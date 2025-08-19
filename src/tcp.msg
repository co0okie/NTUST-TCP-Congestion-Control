packet TcpPacket {
    uint16_t src;
    uint16_t dst;
    uint32_t seq;
    uint32_t ack;
    bool ACK;
    bool SYN;
    bool FIN;
    uint8_t payload[];
}