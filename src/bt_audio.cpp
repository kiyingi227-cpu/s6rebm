#include "bt_audio.h"
#include "board_config.h"

#include <Arduino.h>
#include <string.h>
#include "nvs.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_bt_device.h"
#include "esp_gap_bt_api.h"
#include "esp_a2dp_api.h"
#include "esp_avrc_api.h"

using namespace BoardConfig;

BtAudio *BtAudio::instance_ = nullptr;

static bool macEqual(const uint8_t a[6], const uint8_t b[6]) {
  return memcmp(a, b, 6) == 0;
}

struct BtAudioCallbacks {
  static void a2dData(const uint8_t *data, uint32_t len) {
    BtAudio *self = BtAudio::instance_;
    if (!self || self->stopped_ || !self->output_ || len < 4) return;
    self->lastActivityMs_ = millis();
    self->output_->writeStereoPcm16(reinterpret_cast<const int16_t *>(data), len / 4);
  }

  static void a2dEvent(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param) {
    BtAudio *self = BtAudio::instance_;
    if (!self) return;

    switch (event) {
      case ESP_A2D_CONNECTION_STATE_EVT:
        if (param->conn_stat.state == ESP_A2D_CONNECTION_STATE_CONNECTED) {
          self->connected_        = true;
          self->dualPairOpen_     = false;
          self->lastActivityMs_   = millis();
          self->connectChimePend_ = true;
          memcpy(self->connectedBda_, param->conn_stat.remote_bda, 6);
          self->saveConnectedDevice(param->conn_stat.remote_bda);
          // Hide discoverability once connected (BLUE button opens dual-pair window when wanted)
          esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);
        } else if (param->conn_stat.state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
          self->connected_       = false;
          self->playing_         = false;
          self->clearDmaPending_ = true;
          memset(self->connectedBda_, 0, 6);
          // Immediately jump back to Pairing Mode when no device is connected
          if (!self->stopped_) {
            esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
          }
        }
        break;

      case ESP_A2D_AUDIO_STATE_EVT:
        if (param->audio_stat.state == ESP_A2D_AUDIO_STATE_STARTED) {
          self->playing_        = true;
          self->lastActivityMs_ = millis();
        } else {
          self->playing_         = false;
          self->clearDmaPending_ = true;
        }
        break;

      case ESP_A2D_AUDIO_CFG_EVT:
        if (param->audio_cfg.mcc.type == ESP_A2D_MCT_SBC) {
          uint8_t  oct0 = param->audio_cfg.mcc.cie.sbc[0];
          uint32_t sr   = 44100;
          if (oct0 & 0x10)      sr = 48000;
          else if (oct0 & 0x20) sr = 44100;
          else if (oct0 & 0x40) sr = 32000;
          if (self->output_ && sr != self->output_->sampleRate()) {
            self->pendingSampleRate_ = sr;
          }
        }
        break;

      default:
        break;
    }
  }

  static void avrcCt(esp_avrc_ct_cb_event_t event, esp_avrc_ct_cb_param_t *param) {
    BtAudio *self = BtAudio::instance_;
    if (!self) return;
    if (event == ESP_AVRC_CT_CONNECTION_STATE_EVT) {
      self->avrcCtConnected_ = param->conn_stat.connected;
    }
  }

  static void avrcTg(esp_avrc_tg_cb_event_t event, esp_avrc_tg_cb_param_t *param) {
    BtAudio *self = BtAudio::instance_;
    if (!self || !self->output_) return;

    switch (event) {
      case ESP_AVRC_TG_CONNECTION_STATE_EVT:
        if (!param->conn_stat.connected) self->volNotifyReg_ = false;
        break;

      case ESP_AVRC_TG_SET_ABSOLUTE_VOLUME_CMD_EVT:
        // Per AVRCP 1.6 spec, do NOT fire ESP_AVRC_RN_VOLUME_CHANGE back for a phone-initiated
        // SET_ABSOLUTE_VOLUME — doing so causes Android/iOS volume sliders to jump/skip notches.
        self->output_->setVolume(param->set_abs_vol.volume);
        self->lastActivityMs_ = millis();
        break;

      case ESP_AVRC_TG_REGISTER_NOTIFICATION_EVT:
        if (param->reg_ntf.event_id == ESP_AVRC_RN_VOLUME_CHANGE) {
          self->volNotifyReg_ = true;
          esp_avrc_rn_param_t rn = {};
          rn.volume = self->output_->volume();
          esp_avrc_tg_send_rn_rsp(ESP_AVRC_RN_VOLUME_CHANGE, ESP_AVRC_RN_RSP_INTERIM, &rn);
        }
        break;

      default:
        break;
    }
  }

  static void gap(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param) {
    BtAudio *self = BtAudio::instance_;
    switch (event) {
      case ESP_BT_GAP_PIN_REQ_EVT: {
        esp_bt_pin_code_t pin = {'0', '0', '0', '0'};
        esp_bt_gap_pin_reply(param->pin_req.bda, true, 4, pin);
        break;
      }
      case ESP_BT_GAP_CFM_REQ_EVT:
        esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, true);
        break;

      case ESP_BT_GAP_AUTH_CMPL_EVT:
        // Dual-pair handover: if a 2nd phone pairs while Phone A is connected, hand over cleanly!
        if (self && param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS &&
            self->connected_ && self->dualPairOpen_ &&
            !macEqual(param->auth_cmpl.bda, self->connectedBda_)) {
          memcpy(self->handoverBda_, param->auth_cmpl.bda, 6);
          self->handoverPending_ = true;
        }
        break;

      default:
        break;
    }
  }
};

bool BtAudio::hasValidMac(const uint8_t bda[6]) const {
  for (int i = 0; i < 6; i++) {
    if (bda[i] != 0) return true;
  }
  return false;
}

void BtAudio::loadSavedDevices() {
  nvs_handle_t h;
  if (nvs_open("onyx_bt", NVS_READONLY, &h) != ESP_OK) return;

  size_t sz = 6;
  if (nvs_get_blob(h, "mac0", savedMac_[0], &sz) == ESP_OK && sz == 6 && hasValidMac(savedMac_[0])) {
    hasSavedMac_[0] = true;
  }
  sz = 6;
  if (nvs_get_blob(h, "mac1", savedMac_[1], &sz) == ESP_OK && sz == 6 && hasValidMac(savedMac_[1])) {
    hasSavedMac_[1] = true;
  }
  nvs_close(h);
}

void BtAudio::saveConnectedDevice(const uint8_t bda[6]) {
  if (!hasValidMac(bda)) return;

  // Keep 2 most-recent distinct devices: slot 0 = most recent, slot 1 = previous (for dual-pair swap)
  if (!hasSavedMac_[0] || !macEqual(savedMac_[0], bda)) {
    if (hasSavedMac_[0]) {
      memcpy(savedMac_[1], savedMac_[0], 6);
      hasSavedMac_[1] = true;
    }
    memcpy(savedMac_[0], bda, 6);
    hasSavedMac_[0] = true;

    nvs_handle_t h;
    if (nvs_open("onyx_bt", NVS_READWRITE, &h) == ESP_OK) {
      nvs_set_blob(h, "mac0", savedMac_[0], 6);
      if (hasSavedMac_[1]) nvs_set_blob(h, "mac1", savedMac_[1], 6);
      nvs_commit(h);
      nvs_close(h);
    }
  }
}

bool BtAudio::begin(AudioOutput &output) {
  output_         = &output;
  instance_       = this;
  stopped_        = false;
  lastActivityMs_ = millis();

  loadSavedDevices();

  // btStart() pulls in esp32-hal-bt.c so initArduino() keeps BT RAM (btInUse() == true)
  if (!btStart()) return false;
  if (esp_bluedroid_init() != ESP_OK) return false;
  if (esp_bluedroid_enable() != ESP_OK) return false;

  esp_bt_dev_set_device_name(BT_DEVICE_NAME);
  esp_bt_gap_register_callback(BtAudioCallbacks::gap);

  // SSP "Just Works" pairing (no PIN prompt on modern phones)
  esp_bt_sp_param_t paramType = ESP_BT_SP_IOCAP_MODE;
  esp_bt_io_cap_t   iocap     = ESP_BT_IO_CAP_NONE;
  esp_bt_gap_set_security_param(paramType, &iocap, sizeof(iocap));

  // AVRCP Controller (for PLAY/PAUSE, NEXT, PREV buttons)
  esp_avrc_ct_init();
  esp_avrc_ct_register_callback(BtAudioCallbacks::avrcCt);

  // AVRCP Target (for phone <-> speaker absolute volume sync)
  esp_avrc_tg_init();
  esp_avrc_tg_register_callback(BtAudioCallbacks::avrcTg);
  esp_avrc_rn_evt_cap_mask_t evtSet = {0};
  esp_avrc_rn_evt_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_SET, &evtSet, ESP_AVRC_RN_VOLUME_CHANGE);
  esp_avrc_tg_set_rn_evt_cap(&evtSet);

  // A2DP Sink
  esp_a2d_register_callback(BtAudioCallbacks::a2dEvent);
  esp_a2d_sink_register_data_callback(BtAudioCallbacks::a2dData);
  if (esp_a2d_sink_init() != ESP_OK) return false;

  // Always jump straight into Pairing Mode (connectable + discoverable) when unconnected
  esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);

  // Also attempt a quick auto-reconnect to the last connected phone (stays in pairing mode if absent)
  if (hasSavedMac_[0]) {
    esp_a2d_sink_connect(savedMac_[0]);
  }
  return true;
}

void BtAudio::openDualPairWindow() {
  lastActivityMs_ = millis();
  if (!connected_) {
    // Already unconnected: ensure pairing mode is wide open
    esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
    if (output_) output_->playPairChime();
    return;
  }

  // Connected to Phone A: open 60 s Dual-Pair window so Phone B can pair & take over
  dualPairOpen_    = true;
  dualPairUntilMs_ = millis() + DUAL_PAIR_WINDOW_MS;
  esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
  if (output_) output_->playPairChime();
}

void BtAudio::swapPairedDevice() {
  lastActivityMs_ = millis();
  if (!hasSavedMac_[0] && !hasSavedMac_[1]) {
    openDualPairWindow();
    return;
  }

  // Pick the saved phone that is NOT currently connected
  const uint8_t *target = nullptr;
  if (connected_) {
    if (hasSavedMac_[0] && !macEqual(savedMac_[0], connectedBda_))      target = savedMac_[0];
    else if (hasSavedMac_[1] && !macEqual(savedMac_[1], connectedBda_)) target = savedMac_[1];
  } else {
    target = hasSavedMac_[0] ? savedMac_[0] : savedMac_[1];
  }

  if (!target) {
    openDualPairWindow();
    return;
  }

  if (output_) output_->playPairChime();
  if (connected_) {
    memcpy(nextConnectBda_, target, 6);
    connectAfterDisc_ = true;
    esp_a2d_sink_disconnect(connectedBda_);
  } else {
    esp_a2d_sink_connect(const_cast<uint8_t *>(target));
  }
}

void BtAudio::disconnectAndPair() {
  lastActivityMs_   = millis();
  connectAfterDisc_ = false;
  dualPairOpen_     = false;
  if (output_) output_->playPairChime();
  if (connected_) {
    esp_a2d_sink_disconnect(connectedBda_);
  }
  esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
}

void BtAudio::sendPassthrough(uint8_t keyCode) {
  if (!connected_ || !avrcCtConnected_) return;
  lastActivityMs_ = millis();
  uint8_t tlPress = avrcTl_ & 0x0F;
  avrcTl_ = (avrcTl_ + 1) & 0x0F;
  esp_avrc_ct_send_passthrough_cmd(tlPress, keyCode, ESP_AVRC_PT_CMD_STATE_PRESSED);
  delay(20);
  uint8_t tlRel = avrcTl_ & 0x0F;
  avrcTl_ = (avrcTl_ + 1) & 0x0F;
  esp_avrc_ct_send_passthrough_cmd(tlRel, keyCode, ESP_AVRC_PT_CMD_STATE_RELEASED);
}

void BtAudio::sendPlayPause() {
  sendPassthrough(playing_ ? ESP_AVRC_PT_CMD_PAUSE : ESP_AVRC_PT_CMD_PLAY);
}

void BtAudio::sendNextTrack() {
  sendPassthrough(ESP_AVRC_PT_CMD_FORWARD);
}

void BtAudio::sendPrevTrack() {
  sendPassthrough(ESP_AVRC_PT_CMD_BACKWARD);
}

void BtAudio::notifyVolumeChanged() {
  if (!output_ || !volNotifyReg_) return;
  esp_avrc_rn_param_t rn = {};
  rn.volume = output_->volume();
  esp_avrc_tg_send_rn_rsp(ESP_AVRC_RN_VOLUME_CHANGE, ESP_AVRC_RN_RSP_CHANGED, &rn);
  volNotifyReg_ = false;
}

void BtAudio::poll() {
  if (!output_) return;
  uint32_t now = millis();

  // Close dual-pair window after timeout if no 2nd phone took over
  if (dualPairOpen_ && connected_ && static_cast<int32_t>(now - dualPairUntilMs_) >= 0) {
    dualPairOpen_ = false;
    esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);
  }

  // Dual-pair automatic handover when a 2nd phone completes pairing
  if (handoverPending_) {
    handoverPending_ = false;
    memcpy(nextConnectBda_, handoverBda_, 6);
    connectAfterDisc_ = true;
    dualPairOpen_     = false;
    if (connected_) {
      esp_a2d_sink_disconnect(connectedBda_);
    }
  }

  // Finish queued connection to 2nd phone once Phone A finishes disconnecting
  if (connectAfterDisc_ && !connected_) {
    connectAfterDisc_ = false;
    delay(150);
    esp_a2d_sink_connect(nextConnectBda_);
  }

  // Play soft connect confirmation chime cleanly outside BTC callback
  if (connectChimePend_) {
    connectChimePend_ = false;
    if (!output_->isAuxActive()) {
      output_->playConnectedChime();
    }
  }

  uint32_t reqRate = pendingSampleRate_;
  if (reqRate != 0) {
    pendingSampleRate_ = 0;
    output_->setSampleRate(reqRate);
  }

  if (clearDmaPending_) {
    clearDmaPending_ = false;
    output_->clearDma();
  }
}

void BtAudio::stop() {
  stopped_ = true;
  if (output_) output_->clearDma();
  esp_bt_gap_set_scan_mode(ESP_BT_NON_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);
  esp_a2d_sink_deinit();
  esp_avrc_ct_deinit();
  esp_avrc_tg_deinit();
  connected_ = false;
  playing_   = false;
}
