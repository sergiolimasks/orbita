/* shared test vectors for proto_test.c (C) and proto_golden.cpp (green-nx C++) */
static const char *k_ack_cases[] = {
    "{\"type\":\"HandshakeAck\",\"version\":\"messageV1\",\"id\":\"x\",\"cv\":\"0\"}",
    "{\"type\":\"Handshake\"}",
    "{\"x\":{\"type\":\"HandshakeAck\"}}",
    " {\"cv\":\"0\",\"type\" : \"HandshakeAck\"}",
    "{\"message\":\"type\",\"type\":\"HandshakeAck\"}",
    "{\"type\":\"HandshakeAckX\"}",
    "not json",
    /* "{\"type\":1}" omitted: green-nx throws json type_error (crash); the C port returns 0 */
};
/* {index, b0, b1, LT, RT, LX, LY, RX, RY} for the 24-byte wire */
static const int k_wire[][9] = {
    {0, 0x00, 0x00, 0, 0, 0, 0, 0, 0},
    {0, 0x01, 0x00, 65535, 0, 32767, -32768, 0, 0},
    {1, 0xff, 0x7f, 32768, 65535, -32767, 32767, 1234, -4321},
    {0, 0x50, 0x12, 100, 200, -1, 1, -16384, 16384},
};
/* elapsed values for the gamepad packets */
static const double k_elapsed[] = {0.0, 8.0, 123.456789, 99999.875, 1e9 + 0.5};
