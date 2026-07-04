#include <unity.h>
#include "provisioning_util.h"

void setUp(void) {}
void tearDown(void) {}

void test_valid_locator(void) {
    TEST_ASSERT_TRUE(rover_validate_locator("tcp/192.168.1.42:7447"));
    TEST_ASSERT_TRUE(rover_validate_locator("tcp/hub.local:7447"));
}
void test_reject_no_scheme(void)   { TEST_ASSERT_FALSE(rover_validate_locator("192.168.1.42:7447")); }
void test_reject_no_port(void)     { TEST_ASSERT_FALSE(rover_validate_locator("tcp/192.168.1.42")); }
void test_reject_port_zero(void)   { TEST_ASSERT_FALSE(rover_validate_locator("tcp/host:0")); }
void test_reject_port_huge(void)   { TEST_ASSERT_FALSE(rover_validate_locator("tcp/host:70000")); }
void test_reject_space(void)       { TEST_ASSERT_FALSE(rover_validate_locator("tcp/ho st:7447")); }
void test_reject_empty_host(void)  { TEST_ASSERT_FALSE(rover_validate_locator("tcp/:7447")); }
void test_reject_overlong(void) {
    char big[80]; for (int i=0;i<79;i++) big[i]='a'; big[79]=0;
    TEST_ASSERT_FALSE(rover_validate_locator(big));
}
void test_robot_id_format(void) {
    uint8_t mac[6] = {0x24,0x6f,0x28,0xaa,0x0d,0x08};
    char id[16]; rover_format_robot_id(mac, id);
    TEST_ASSERT_EQUAL_STRING("rover-0d08", id);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_valid_locator);
    RUN_TEST(test_reject_no_scheme);
    RUN_TEST(test_reject_no_port);
    RUN_TEST(test_reject_port_zero);
    RUN_TEST(test_reject_port_huge);
    RUN_TEST(test_reject_space);
    RUN_TEST(test_reject_empty_host);
    RUN_TEST(test_reject_overlong);
    RUN_TEST(test_robot_id_format);
    return UNITY_END();
}
