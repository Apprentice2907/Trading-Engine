#include "test_framework.hpp"

int main() {
    return test::TestRegistry::instance().run_all();
}
