// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <memory>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "dynarmic/interface/A64/a64.h"
#include "dynarmic/interface/exclusive_monitor.h"
#include "dynarmic/interface/optimization_flags.h"
#include "dynarmic/tests/A64/testenv.h"
#include "dynarmic/tests/native/testenv.h"

using namespace Dynarmic;

namespace {

constexpr u64 data_address = 0x10000;
constexpr u32 page_table_bits = 20;

// Exclusives reach memory through the page table (no fastmem) and skip the monitor's lock.
struct InlineExclusiveSetup {
    std::vector<void*> page_table = std::vector<void*>(1ULL << (page_table_bits - 12));
    std::vector<u64> data = std::vector<u64>(4096 / sizeof(u64));

    InlineExclusiveSetup() {
        page_table[data_address >> 12] = data.data();
    }

    A64::UserConfig Config(A64TestEnv& env, ExclusiveMonitor& monitor, std::size_t processor_id) {
        A64::UserConfig conf{};
        conf.callbacks = &env;
        conf.global_monitor = &monitor;
        conf.processor_id = static_cast<std::uint8_t>(processor_id);
        conf.page_table = page_table.data();
        conf.page_table_address_space_bits = page_table_bits;
        conf.unsafe_optimizations = true;
        conf.optimizations |= OptimizationFlag::Unsafe_IgnoreGlobalMonitor;
        return conf;
    }
};

}  // namespace

TEST_CASE("A64: exclusives through the page table", "[a64]") {
    InlineExclusiveSetup setup;
    A64TestEnv env;
    ExclusiveMonitor monitor{1};
    A64::Jit jit{setup.Config(env, monitor, 0)};

    env.code_mem.emplace_back(0xc85ffc01);  // LDAXR X1, [X0]
    env.code_mem.emplace_back(0x91000421);  // ADD X1, X1, #1
    env.code_mem.emplace_back(0xc802fc01);  // STLXR W2, X1, [X0]
    env.code_mem.emplace_back(0xd503305f);  // CLREX
    env.code_mem.emplace_back(0xc8067c01);  // STXR W6, X1, [X0]
    env.code_mem.emplace_back(0x085ffd07);  // LDAXRB W7, [X8]
    env.code_mem.emplace_back(0x0809fd0a);  // STLXRB W9, W10, [X8]
    env.code_mem.emplace_back(0xc8ebfc0c);  // CASAL X11, X12, [X0]
    env.code_mem.emplace_back(0xc85ffdae);  // LDAXR X14, [X13]
    env.code_mem.emplace_back(0xc80ffdae);  // STLXR W15, X14, [X13]
    env.code_mem.emplace_back(0x14000000);  // B .

    setup.data[0] = 41;
    setup.data[2] = 0x7f;
    jit.SetPC(0);
    jit.SetRegister(0, data_address);
    jit.SetRegister(6, 0xff);
    jit.SetRegister(8, data_address + 16);
    jit.SetRegister(9, 0xff);
    jit.SetRegister(10, 0x55);
    jit.SetRegister(11, 42);
    jit.SetRegister(12, 100);
    jit.SetRegister(13, 0x20000);  // Not in the page table: the monitor's callbacks.
    jit.SetRegister(15, 0xff);

    env.ticks_left = 20;
    CheckedRun([&]() { jit.Run(); });

    REQUIRE(jit.GetRegister(1) == 42);
    REQUIRE(jit.GetRegister(2) == 0);
    REQUIRE(jit.GetRegister(6) == 1);
    REQUIRE(jit.GetRegister(7) == 0x7f);
    REQUIRE(jit.GetRegister(9) == 0);
    REQUIRE(jit.GetRegister(11) == 42);
    REQUIRE(jit.GetRegister(15) == 0);
    REQUIRE(setup.data[0] == 100);
    REQUIRE(setup.data[2] == 0x55);
}

TEST_CASE("A64: exclusives through the page table are atomic across cores", "[a64]") {
    constexpr std::size_t cores = 4;
    constexpr u64 iterations = 100'000;

    InlineExclusiveSetup setup;
    ExclusiveMonitor monitor{cores};
    std::array<A64TestEnv, cores> envs;
    std::vector<std::unique_ptr<A64::Jit>> jits;

    for (std::size_t i = 0; i < cores; i++) {
        auto& env = envs[i];
        env.code_mem.emplace_back(0xc85ffc01);  // LDAXR X1, [X0]
        env.code_mem.emplace_back(0x91000421);  // ADD X1, X1, #1
        env.code_mem.emplace_back(0xc802fc01);  // STLXR W2, X1, [X0]
        env.code_mem.emplace_back(0x35ffffa2);  // CBNZ W2, #-12
        env.code_mem.emplace_back(0xf8e40125);  // LDADDAL X4, X5, [X9]
        env.code_mem.emplace_back(0xd1000463);  // SUB X3, X3, #1
        env.code_mem.emplace_back(0xb5ffff43);  // CBNZ X3, #-24
        env.code_mem.emplace_back(0x14000000);  // B .
        env.ticks_left = iterations * 64;

        auto& jit = jits.emplace_back(std::make_unique<A64::Jit>(setup.Config(env, monitor, i)));
        jit->SetPC(0);
        jit->SetRegister(0, data_address);
        jit->SetRegister(3, iterations);
        jit->SetRegister(4, 1);
        jit->SetRegister(9, data_address + 8);
    }

    std::vector<std::thread> threads;
    for (auto& jit : jits) {
        threads.emplace_back([&jit] { CheckedRun([&]() { jit->Run(); }); });
    }
    for (auto& thread : threads) {
        thread.join();
    }

    for (auto& jit : jits) {
        REQUIRE(jit->GetRegister(3) == 0);
    }
    REQUIRE(setup.data[0] == cores * iterations);
    REQUIRE(setup.data[1] == cores * iterations);
}
