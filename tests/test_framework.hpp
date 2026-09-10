#pragma once

#include <iostream>
#include <vector>
#include <string>
#include <functional>
#include <sstream>
#include <chrono>

namespace test {

struct TestFailure {
    std::string file;
    int line;
    std::string condition;
    std::string message;
};

class TestCase {
public:
    TestCase(std::string name, std::function<void()> func)
        : name_(std::move(name)), func_(std::move(func)) {}

    void run() {
        failures_.clear();
        try {
            func_();
        } catch (const std::exception& e) {
            record_failure(__FILE__, __LINE__, "Unhandled Exception", e.what());
        } catch (...) {
            record_failure(__FILE__, __LINE__, "Unhandled Exception", "Unknown non-std exception");
        }
    }

    void record_failure(std::string file, int line, std::string cond, std::string msg = "") {
        failures_.push_back(TestFailure{std::move(file), line, std::move(cond), std::move(msg)});
    }

    [[nodiscard]] const std::string& name() const noexcept { return name_; }
    [[nodiscard]] bool passed() const noexcept { return failures_.empty(); }
    [[nodiscard]] const std::vector<TestFailure>& failures() const noexcept { return failures_; }

private:
    std::string name_;
    std::function<void()> func_;
    std::vector<TestFailure> failures_;
};

class TestRegistry {
public:
    static TestRegistry& instance() {
        static TestRegistry reg;
        return reg;
    }

    void register_test(std::string name, std::function<void()> func) {
        tests_.emplace_back(std::move(name), std::move(func));
    }

    TestCase* current_test() {
        return current_test_;
    }

    int run_all() {
        std::cout << "==================================================\n";
        std::cout << " RUNNING HFT TEST SUITE (" << tests_.size() << " test cases)\n";
        std::cout << "==================================================\n\n";

        size_t passed_count = 0;
        size_t failed_count = 0;

        const auto start_time = std::chrono::steady_clock::now();

        for (auto& tc : tests_) {
            current_test_ = &tc;
            std::cout << "[ RUN      ] " << tc.name() << "\n";
            tc.run();
            if (tc.passed()) {
                std::cout << "[       OK ] " << tc.name() << "\n";
                passed_count++;
            } else {
                std::cout << "[  FAILED  ] " << tc.name() << "\n";
                for (const auto& f : tc.failures()) {
                    std::cout << "  --> " << f.file << ":" << f.line
                              << " - Assertion failed: " << f.condition;
                    if (!f.message.empty()) {
                        std::cout << " (" << f.message << ")";
                    }
                    std::cout << "\n";
                }
                failed_count++;
            }
        }

        const auto end_time = std::chrono::steady_clock::now();
        const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();

        std::cout << "\n==================================================\n";
        std::cout << " TEST SUMMARY\n";
        std::cout << " Total:  " << tests_.size() << "\n";
        std::cout << " Passed: " << passed_count << "\n";
        std::cout << " Failed: " << failed_count << "\n";
        std::cout << " Time:   " << elapsed_ms << " ms\n";
        std::cout << "==================================================\n";

        return failed_count == 0 ? 0 : 1;
    }

private:
    std::vector<TestCase> tests_;
    TestCase* current_test_{nullptr};
};

struct AutoRegister {
    AutoRegister(std::string name, std::function<void()> func) {
        TestRegistry::instance().register_test(std::move(name), std::move(func));
    }
};

template <typename T>
concept Streamable = requires(std::ostream& os, const T& t) {
    os << t;
};

template <typename T>
std::string format_val(const T& val) {
    if constexpr (Streamable<T>) {
        std::ostringstream oss;
        oss << val;
        return oss.str();
    } else {
        return "<unstreamable>";
    }
}

} // namespace test

#define TEST_CASE(name) \
    static void test_func_##name(); \
    static ::test::AutoRegister auto_reg_##name(#name, test_func_##name); \
    static void test_func_##name()

#define ASSERT_TRUE(cond) \
    do { \
        if (!(cond)) { \
            if (auto* tc = ::test::TestRegistry::instance().current_test()) { \
                tc->record_failure(__FILE__, __LINE__, #cond); \
            } \
            return; \
        } \
    } while (false)

#define ASSERT_FALSE(cond) \
    do { \
        if (cond) { \
            if (auto* tc = ::test::TestRegistry::instance().current_test()) { \
                tc->record_failure(__FILE__, __LINE__, "!(" #cond ")"); \
            } \
            return; \
        } \
    } while (false)

#define ASSERT_EQ(val1, val2) \
    do { \
        auto v1 = (val1); \
        auto v2 = (val2); \
        if (v1 != v2) { \
            std::ostringstream oss; \
            oss << "Expected " #val1 " (" << ::test::format_val(v1) << ") == " #val2 " (" << ::test::format_val(v2) << ")"; \
            if (auto* tc = ::test::TestRegistry::instance().current_test()) { \
                tc->record_failure(__FILE__, __LINE__, #val1 " == " #val2, oss.str()); \
            } \
            return; \
        } \
    } while (false)

#define ASSERT_NE(val1, val2) \
    do { \
        auto v1 = (val1); \
        auto v2 = (val2); \
        if (v1 == v2) { \
            std::ostringstream oss; \
            oss << "Expected " #val1 " (" << ::test::format_val(v1) << ") != " #val2 " (" << ::test::format_val(v2) << ")"; \
            if (auto* tc = ::test::TestRegistry::instance().current_test()) { \
                tc->record_failure(__FILE__, __LINE__, #val1 " != " #val2, oss.str()); \
            } \
            return; \
        } \
    } while (false)

#define ASSERT_LT(val1, val2) \
    do { \
        auto v1 = (val1); \
        auto v2 = (val2); \
        if (!(v1 < v2)) { \
            std::ostringstream oss; \
            oss << "Expected " #val1 " (" << ::test::format_val(v1) << ") < " #val2 " (" << ::test::format_val(v2) << ")"; \
            if (auto* tc = ::test::TestRegistry::instance().current_test()) { \
                tc->record_failure(__FILE__, __LINE__, #val1 " < " #val2, oss.str()); \
            } \
            return; \
        } \
    } while (false)

#define ASSERT_GT(val1, val2) \
    do { \
        auto v1 = (val1); \
        auto v2 = (val2); \
        if (!(v1 > v2)) { \
            std::ostringstream oss; \
            oss << "Expected " #val1 " (" << ::test::format_val(v1) << ") > " #val2 " (" << ::test::format_val(v2) << ")"; \
            if (auto* tc = ::test::TestRegistry::instance().current_test()) { \
                tc->record_failure(__FILE__, __LINE__, #val1 " > " #val2, oss.str()); \
            } \
            return; \
        } \
    } while (false)

#define ASSERT_LE(val1, val2) \
    do { \
        auto v1 = (val1); \
        auto v2 = (val2); \
        if (!(v1 <= v2)) { \
            std::ostringstream oss; \
            oss << "Expected " #val1 " (" << ::test::format_val(v1) << ") <= " #val2 " (" << ::test::format_val(v2) << ")"; \
            if (auto* tc = ::test::TestRegistry::instance().current_test()) { \
                tc->record_failure(__FILE__, __LINE__, #val1 " <= " #val2, oss.str()); \
            } \
            return; \
        } \
    } while (false)

#define ASSERT_GE(val1, val2) \
    do { \
        auto v1 = (val1); \
        auto v2 = (val2); \
        if (!(v1 >= v2)) { \
            std::ostringstream oss; \
            oss << "Expected " #val1 " (" << ::test::format_val(v1) << ") >= " #val2 " (" << ::test::format_val(v2) << ")"; \
            if (auto* tc = ::test::TestRegistry::instance().current_test()) { \
                tc->record_failure(__FILE__, __LINE__, #val1 " >= " #val2, oss.str()); \
            } \
            return; \
        } \
    } while (false)


