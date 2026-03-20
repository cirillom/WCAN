# WCAN – Wireless CAN Bridge for Formula Student

## Goal
Create a **drop‑in, wire‑less extension of the car’s CAN bus** so that sensors mounted in hard‑to‑reach places can be read without changing the existing electrical architecture. Each sensor node transmits its data over **ESPNOW** (WiFi) to *N* receiver nodes; the receiver injects the data on the wired CAN bus as if it had come from any other wired sensor.

## Requirements
### Hardware
- **Sensor node**
  - [Beetle ESP32 C6 module](https://www.dfrobot.com/product-2778.html)
  - The actual sensor (analogue or digital)
  - Li-ion 4.2V battery
- **Receiver node**
  - ESP32‑WROOM module
  - STM L9616 CAN transceiver (compatible with 1 Mbit/s)
  - Connection to the car’s 120 Ω terminated CAN lines (H & L)

### Software / Toolchain
- **ESP‑IDF v5.4.1**
- Altium 24.8.2

## How It Works
```
 ┌──────────────┐       ESPNOW          ┌──────────────┐        CAN bus
 │  Sensor ESP  │  ───────────────►    │ Receiver ESP │  ───────►  Car ECU
 └──────────────┘   (broadcast)        └──────────────┘
```
1. **Frame build** – Each sensor packs its value into an 11‑bit *CAN ID* + up to 8‑byte *payload*.
2. **Broadcast & wait** – The frame is broadcast over ESPNOW; the node then waits for an **ACK**.
3. **Retransmit logic** – If the ACK is not seen, the node retransmits up to *MAX_RETRY* times. After that the message is counted as **lost**.
4. **Receive & forward** – The receiver filters incoming ESPNOW frames (by CAN ID mask), sends the ACK back, converts the frame to a native CAN packet and transmits it with the same ID/payload on the wired bus.
5. **Error integrity** – Because the transmitter retransmits until acknowledged, the wired side still benefits from CAN’s error detection (CRC, ACK‑slot). Lost wireless frames are detected but simply never placed on the bus, preserving real‑time behaviour.

## Project Structure
```
.
├── Documentation/
│   ├── Code.canvas
│   ├── Flow.canvas
│   └── Wireless Controller Area Network.md
├── transmitter/
│   ├── components/
│   │   ├── utils/
│   │   └── wcan/
│   │       ├── inc/
│   │       │   ├── wcan_communication.h
│   │       │   ├── wcan_receiver.h
│   │       │   ├── wcan_sender.h
│   │       │   └── wcan_utils.h
│   │       └── src/
│   │           ├── wcan_communication.cpp
│   │           ├── wcan_receiver.cpp
│   │           ├── wcan_sender.cpp
│   │           └── wcan_utils.cpp
│   ├── main/
│   └── CMakeLists.txt
├── WCAN_Template_Shield/
│   ├── connectors.SchDoc
│   ├── shield_template.PcbDoc
│   └── WCAN_Template_Shield.PrjPcb
└── README.md
```

## License
MIT – see [LICENSE](LICENSE) for details.

## Authors
* [Matheus Cirillo](https://github.com/cirillom)
