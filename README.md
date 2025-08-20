# TCP Congestion Control Experiment

## To DO

- 三次握手、四次揮手
- 流量控制: 目前 swnd 與 rwnd 皆為定值，應在 `tcp.msg` 中新增 `uint16_t window` 以傳送自己的 rwnd，並使 swnd 等於所收到的 rwnd
- 動態RTO: 目前為固定時間，應紀錄 SRTT (Smooth RTT) 與 RTTVAR (RTT Variance)，並依照標準以 1/8 與 1/4 的比例更新
- 壅塞控制
  - slow start & congestion avoidance
  - RENO (fast retransmit & fast recovery)
  - Cubic
  - BBR
  - BBRv2