# BLE-Audio-Profile + Ambient Noise Monitoring (nRF5340 + Zephyr)


---

## 📡 Overview

This application demonstrates **Bluetooth LE Audio unicast streaming** using the **nRF5340 Audio DK**, extended with **ambient noise monitoring** for future integration of **active noise cancellation (ANC)** features. It runs under the **Zephyr Real-Time Operating System** and is designed for real-time audio performance without interrupting BLE connectivity.

> 🔧 Built as part of an embedded audiometric diagnostic system at **dB.Sense**, a spin-off from the University of Tunis El Manar.

---

## 🚀 Features

* ✅ BLE Audio unicast streaming (client/server) based on **Nordic nRF Connect SDK**
* ✅ **LC3 codec** integration with ISO channels for low-latency audio
* ✅ **Ambient noise monitoring thread**:

  * Captures real-time mic data via **I²S**
  * Computes **RMS noise levels**
  * Triggers LED feedback if threshold exceeded
* ✅ Designed for **future ANC integration**
* ✅ Runs under **Zephyr RTOS** for deterministic, multi-threaded execution

---

## 🔧 Technologies Used

| Component             | Description                                                                                                                |
| --------------------- | -------------------------------------------------------------------------------------------------------------------------- |
| **Hardware**          | [nRF5340 Audio DK](https://www.nordicsemi.com/Products/Development-hardware/nrf5340-audio-dk)                              |
| **RTOS**              | [Zephyr Project](https://zephyrproject.org/) (via nRF Connect SDK)                                                         |
| **Audio Codec**       | [LC3](https://www.bluetooth.com/learn-about-bluetooth/bluetooth-technology/le-audio/) – Low Complexity Communication Codec |
| **Bluetooth Profile** | BLE Audio (BAP Unicast) via Nordic stack                                                                                   |
| **Mic Interface**     | I²S + MEMS digital microphone                                                                                              |

---

## 🧠 Core Contribution: Ambient Noise Monitoring

A custom thread (`noise_level_thread`) was implemented within the existing `audio_system.c`:

* 🧩 **Integrated into Zephyr’s thread model** alongside BLE streaming
* 📉 **RMS noise estimation** over 64-sample windows (1.33ms @ 48kHz)
* 🔴 **LED feedback** via GPIO when noise exceeds a predefined threshold
* ⚙️ Non-blocking FIFO access to avoid CPU starvation
* 🕒 Sampling interval: every 1s (can be optimized for lower latency)

### Known Limitations

* **Short noise spikes** may be missed between sampling intervals
* **Fixed noise threshold** (empirical, no dB SPL calibration yet)
* Further optimization needed for dynamic environments

---

The ambient noise thread will automatically start when bidirectional mode is active on the **unicast server**.

---


