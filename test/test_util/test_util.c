#include <unity.h>
#include "provisioning_util.h"

void setUp(void) {}
void tearDown(void) {}

void test_valid_locator(void) {
    TEST_ASSERT_TRUE(rover_validate_locator("mqtt://192.168.1.42:1883"));
    TEST_ASSERT_TRUE(rover_validate_locator("mqtt://hub.local:1883"));
}
void test_reject_no_scheme(void)   { TEST_ASSERT_FALSE(rover_validate_locator("192.168.1.42:1883")); }
void test_reject_old_scheme(void)  { TEST_ASSERT_FALSE(rover_validate_locator("tcp/192.168.1.42:7447")); }
void test_reject_no_port(void)     { TEST_ASSERT_FALSE(rover_validate_locator("mqtt://192.168.1.42")); }
void test_reject_port_zero(void)   { TEST_ASSERT_FALSE(rover_validate_locator("mqtt://host:0")); }
void test_reject_port_huge(void)   { TEST_ASSERT_FALSE(rover_validate_locator("mqtt://host:70000")); }
void test_reject_space(void)       { TEST_ASSERT_FALSE(rover_validate_locator("mqtt://ho st:1883")); }
void test_reject_empty_host(void)  { TEST_ASSERT_FALSE(rover_validate_locator("mqtt://:1883")); }
void test_reject_overlong(void) {
    char big[80]; for (int i=0;i<79;i++) big[i]='a'; big[79]=0;
    TEST_ASSERT_FALSE(rover_validate_locator(big));
}
// Hub admission: unpinned = any hub-*; pinned = that exact SSID only.
void test_admits_unpinned(void) {
    TEST_ASSERT_TRUE (rover_hub_admits("hub-a045", NULL));
    TEST_ASSERT_TRUE (rover_hub_admits("hub-pi-3f2a", ""));
    TEST_ASSERT_FALSE(rover_hub_admits("rover-b79c", NULL));   // islands are not hubs
    TEST_ASSERT_FALSE(rover_hub_admits("HomeNet", ""));
    TEST_ASSERT_FALSE(rover_hub_admits(NULL, NULL));
}
void test_admits_pinned(void) {
    TEST_ASSERT_TRUE (rover_hub_admits("hub-a045", "hub-a045"));
    TEST_ASSERT_FALSE(rover_hub_admits("hub-9999", "hub-a045"));  // foreign hub refused
    TEST_ASSERT_FALSE(rover_hub_admits("hub-a0455", "hub-a045")); // exact, not prefix
    TEST_ASSERT_FALSE(rover_hub_admits("rover-x", "rover-x"));    // a pin can't bless a non-hub
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
    RUN_TEST(test_reject_old_scheme);
    RUN_TEST(test_reject_no_port);
    RUN_TEST(test_reject_port_zero);
    RUN_TEST(test_reject_port_huge);
    RUN_TEST(test_reject_space);
    RUN_TEST(test_reject_empty_host);
    RUN_TEST(test_reject_overlong);
    RUN_TEST(test_admits_unpinned);
    RUN_TEST(test_admits_pinned);
    RUN_TEST(test_robot_id_format);
    return UNITY_END();
}
