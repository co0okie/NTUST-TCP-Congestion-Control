packet TcpPacket {
    uint32_t seq;
    uint32_t ack;
    bool ACK;
    uint32_t window;
    uint8_t payload[];
}