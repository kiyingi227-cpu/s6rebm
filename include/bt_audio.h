#pragma once
#include <stdint.h>
#include "audio_output.h"

class BtAudio {
public:
  bool begin(AudioOutput &output);
  void poll();
  void stop();

  // Front button & dual-pair actions
  void openDualPairWindow();
  void swapPairedDevice();
  void disconnectAndPair();

  // AVRCP media & volume control
  void sendPlayPause();
  void sendNextTrack();
  void sendPrevTrack();
  void notifyVolumeChanged();

  bool     isConnected()    const { return connected_; }
  bool     isPlaying()      const { return playing_; }
  bool     isDualPairOpen() const { return dualPairOpen_; }
  uint32_t lastActivityMs() const { return lastActivityMs_; }

private:
  void loadSavedDevices();
  void saveConnectedDevice(const uint8_t bda[6]);
  bool hasValidMac(const uint8_t bda[6]) const;
  void sendPassthrough(uint8_t keyCode);

  static BtAudio *instance_;

  AudioOutput      *output_            = nullptr;
  volatile bool     stopped_           = false;
  volatile bool     connected_         = false;
  volatile bool     playing_           = false;
  volatile bool     avrcCtConnected_   = false;
  volatile bool     volNotifyReg_      = false;
  volatile bool     clearDmaPending_   = false;
  volatile bool     connectChimePend_  = false;
  volatile uint32_t pendingSampleRate_ = 0;
  volatile uint32_t lastActivityMs_    = 0;

  // Dual-pair / 2-slot paired device management
  uint8_t  connectedBda_[6]  = {0};
  uint8_t  savedMac_[2][6]   = {{0}, {0}};
  bool     hasSavedMac_[2]   = {false, false};
  bool     dualPairOpen_     = false;
  uint32_t dualPairUntilMs_  = 0;

  // Deferred handover / swap requests (executed cleanly in poll())
  volatile bool handoverPending_   = false;
  uint8_t       handoverBda_[6]    = {0};
  bool          connectAfterDisc_  = false;
  uint8_t       nextConnectBda_[6] = {0};
  uint8_t       avrcTl_            = 0;

  friend struct BtAudioCallbacks;
};
