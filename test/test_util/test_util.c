#include <unity.h>
#include "provisioning_util.h"

void setUp(void) {}
void tearDown(void) {}

void test_valid_locator(void) {
    TEST_ASSERT_TRUE(robot_validate_locator("mqtt://192.168.1.42:1883"));
    TEST_ASSERT_TRUE(robot_validate_locator("mqtt://hub.local:1883"));
}
void test_reject_no_scheme(void)   { TEST_ASSERT_FALSE(robot_validate_locator("192.168.1.42:1883")); }
void test_reject_old_scheme(void)  { TEST_ASSERT_FALSE(robot_validate_locator("tcp/192.168.1.42:7447")); }
void test_reject_no_port(void)     { TEST_ASSERT_FALSE(robot_validate_locator("mqtt://192.168.1.42")); }
void test_reject_port_zero(void)   { TEST_ASSERT_FALSE(robot_validate_locator("mqtt://host:0")); }
void test_reject_port_huge(void)   { TEST_ASSERT_FALSE(robot_validate_locator("mqtt://host:70000")); }
void test_reject_space(void)       { TEST_ASSERT_FALSE(robot_validate_locator("mqtt://ho st:1883")); }
void test_reject_empty_host(void)  { TEST_ASSERT_FALSE(robot_validate_locator("mqtt://:1883")); }
void test_reject_overlong(void) {
    char big[80]; for (int i=0;i<79;i++) big[i]='a'; big[79]=0;
    TEST_ASSERT_FALSE(robot_validate_locator(big));
}
// Hub admission: unpinned = any hub-*; pinned = that exact SSID only.
void test_admits_unpinned(void) {
    TEST_ASSERT_TRUE (robot_hub_admits("hub-a045", NULL));
    TEST_ASSERT_TRUE (robot_hub_admits("hub-pi-3f2a", ""));
    TEST_ASSERT_FALSE(robot_hub_admits("robot-b79c", NULL));   // islands are not hubs
    TEST_ASSERT_FALSE(robot_hub_admits("HomeNet", ""));
    TEST_ASSERT_FALSE(robot_hub_admits(NULL, NULL));
}
void test_admits_pinned(void) {
    TEST_ASSERT_TRUE (robot_hub_admits("hub-a045", "hub-a045"));
    TEST_ASSERT_FALSE(robot_hub_admits("hub-9999", "hub-a045"));  // foreign hub refused
    TEST_ASSERT_FALSE(robot_hub_admits("hub-a0455", "hub-a045")); // exact, not prefix
    TEST_ASSERT_FALSE(robot_hub_admits("robot-x", "robot-x"));    // a pin can't bless a non-hub
}

void test_robot_id_format(void) {
    uint8_t mac[6] = {0x24,0x6f,0x28,0xaa,0x0d,0x08};
    char id[16]; robot_format_robot_id(mac, id);
    TEST_ASSERT_EQUAL_STRING("robot-0d08", id);
}

// Host discriminator: OUR origins (IP literal / *.local / bare / empty) are
// local; a public dotted name is a probe (→ /welcome) or a rebind (→ 403).
void test_host_local(void) {
    TEST_ASSERT_TRUE (robot_host_is_local("192.168.99.1"));      // AP IP
    TEST_ASSERT_TRUE (robot_host_is_local("192.168.99.1:80"));   // IP with port
    TEST_ASSERT_TRUE (robot_host_is_local("robot-a044.local"));  // mDNS
    TEST_ASSERT_TRUE (robot_host_is_local("robot.local"));       // mDNS alias
    TEST_ASSERT_TRUE (robot_host_is_local("ROBOT.LOCAL"));       // case-insensitive
    TEST_ASSERT_TRUE (robot_host_is_local("hub"));               // bare single label
    TEST_ASSERT_TRUE (robot_host_is_local(""));                  // no Host
    TEST_ASSERT_TRUE (robot_host_is_local(NULL));
}
void test_host_foreign(void) {
    TEST_ASSERT_FALSE(robot_host_is_local("captive.apple.com"));      // Apple probe
    TEST_ASSERT_FALSE(robot_host_is_local("connectivitycheck.gstatic.com"));
    TEST_ASSERT_FALSE(robot_host_is_local("attacker.com"));           // rebind pivot
    TEST_ASSERT_FALSE(robot_host_is_local("evil.local.attacker.com")); // .local not the suffix
    TEST_ASSERT_FALSE(robot_host_is_local("localhost.evil.com"));
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
    RUN_TEST(test_host_local);
    RUN_TEST(test_host_foreign);
    return UNITY_END();
}
