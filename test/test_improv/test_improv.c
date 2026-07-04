#include <string.h>
#include <unity.h>
#include "improv.h"

void setUp(void) {}
void tearDown(void) {}

static void make_wifi(uint8_t *pkt, size_t *n, const char *ssid, const char *pass) {
    size_t sl = strlen(ssid), pl = strlen(pass);
    uint8_t data[80]; size_t d = 0;
    data[d++] = (uint8_t)sl; memcpy(data+d, ssid, sl); d += sl;
    data[d++] = (uint8_t)pl; memcpy(data+d, pass, pl); d += pl;
    size_t i = 0; pkt[i++] = IMPROV_CMD_SEND_WIFI; pkt[i++] = (uint8_t)d;
    memcpy(pkt+i, data, d); i += d;
    pkt[i] = improv_checksum(pkt, i); i++; *n = i;
}

void test_checksum_low_byte_sum(void) {
    uint8_t a[] = {0x01, 0xFF}; TEST_ASSERT_EQUAL_HEX8(0x00, improv_checksum(a, 2));
    uint8_t b[] = {0x04, 0x00}; TEST_ASSERT_EQUAL_HEX8(0x04, improv_checksum(b, 2));
}

void test_parse_scan(void) {
    uint8_t pkt[] = {0x04, 0x00, 0x04};
    improv_command_t c;
    TEST_ASSERT_EQUAL(IMPROV_OK, improv_parse_command(pkt, sizeof pkt, &c));
    TEST_ASSERT_EQUAL_UINT8(IMPROV_CMD_SCAN_WIFI, c.cmd);
}

void test_parse_send_wifi(void) {
    uint8_t pkt[64]; size_t n; make_wifi(pkt, &n, "hi", "pw");
    improv_command_t c;
    TEST_ASSERT_EQUAL(IMPROV_OK, improv_parse_command(pkt, n, &c));
    TEST_ASSERT_EQUAL_UINT8(IMPROV_CMD_SEND_WIFI, c.cmd);
    TEST_ASSERT_EQUAL_STRING("hi", c.ssid);
    TEST_ASSERT_EQUAL_STRING("pw", c.pass);
}

void test_open_network_empty_pass(void) {
    uint8_t pkt[64]; size_t n; make_wifi(pkt, &n, "hi", "");
    improv_command_t c;
    TEST_ASSERT_EQUAL(IMPROV_OK, improv_parse_command(pkt, n, &c));
    TEST_ASSERT_EQUAL_STRING("", c.pass);
}

void test_reject_flag_smuggling_ssid(void) {
    uint8_t pkt[64]; size_t n; make_wifi(pkt, &n, "-x", "");
    improv_command_t c;
    TEST_ASSERT_EQUAL(IMPROV_ERR_INVALID, improv_parse_command(pkt, n, &c));
}

void test_reject_control_chars(void) {
    uint8_t pkt[64]; size_t n; make_wifi(pkt, &n, "ne\nt", "");
    improv_command_t c;
    TEST_ASSERT_EQUAL(IMPROV_ERR_INVALID, improv_parse_command(pkt, n, &c));
}

void test_reject_oversized_ssid(void) {
    char big[34]; memset(big, 'a', 33); big[33] = 0;
    uint8_t pkt[80]; size_t n; make_wifi(pkt, &n, big, "");
    improv_command_t c;
    TEST_ASSERT_EQUAL(IMPROV_ERR_INVALID, improv_parse_command(pkt, n, &c));
}

void test_reject_bad_checksum(void) {
    uint8_t pkt[] = {0x04, 0x00, 0xFF};
    improv_command_t c;
    TEST_ASSERT_EQUAL(IMPROV_ERR_INVALID, improv_parse_command(pkt, sizeof pkt, &c));
}

void test_reject_length_mismatch(void) {
    uint8_t pkt[] = {0x01, 0x05, 0x00, 0x00};
    improv_command_t c;
    TEST_ASSERT_EQUAL(IMPROV_ERR_INVALID, improv_parse_command(pkt, sizeof pkt, &c));
}

void test_unknown_command(void) {
    uint8_t pkt[] = {0x06, 0x00, 0x06};
    improv_command_t c;
    TEST_ASSERT_EQUAL(IMPROV_ERR_UNKNOWN_CMD, improv_parse_command(pkt, sizeof pkt, &c));
}

void test_encode_empty_scan_result(void) {
    uint8_t out[16];
    size_t n = improv_encode_result(IMPROV_CMD_SCAN_WIFI, NULL, 0, out, sizeof out);
    TEST_ASSERT_EQUAL_UINT(3, n);
    uint8_t expect[] = {0x04, 0x00, 0x04};
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expect, out, 3);
}

void test_encode_one_string(void) {
    const char *s[] = {"ok"};
    uint8_t out[16];
    size_t n = improv_encode_result(IMPROV_CMD_DEVICE_INFO, s, 1, out, sizeof out);
    uint8_t head[] = {0x03, 0x03, 0x02, 'o', 'k'};
    TEST_ASSERT_EQUAL_HEX8_ARRAY(head, out, 5);
    TEST_ASSERT_EQUAL_HEX8(improv_checksum(out, n-1), out[n-1]);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_checksum_low_byte_sum);
    RUN_TEST(test_parse_scan);
    RUN_TEST(test_parse_send_wifi);
    RUN_TEST(test_open_network_empty_pass);
    RUN_TEST(test_reject_flag_smuggling_ssid);
    RUN_TEST(test_reject_control_chars);
    RUN_TEST(test_reject_oversized_ssid);
    RUN_TEST(test_reject_bad_checksum);
    RUN_TEST(test_reject_length_mismatch);
    RUN_TEST(test_unknown_command);
    RUN_TEST(test_encode_empty_scan_result);
    RUN_TEST(test_encode_one_string);
    return UNITY_END();
}
