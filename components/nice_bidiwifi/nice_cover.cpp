#include <algorithm>
#include "nice_cover.h"
#include "esphome/core/log.h"

namespace esphome {
namespace nice_bidiwifi {

static const char *const TAG = "nice_bidiwifi.cover";

void NiceCover::setup() {
  ESP_LOGCONFIG(TAG, "Setting up Nice Cover...");
  this->hub_->add_on_state_callback([this]() { this->on_state_change_(); });
}

void NiceCover::dump_config() {
  LOG_COVER("", "Nice BiDi-WiFi Cover", this);
}

cover::CoverTraits NiceCover::get_traits() {
  auto traits = cover::CoverTraits();
  traits.set_supports_position(true);
  traits.set_supports_stop(true);
  traits.set_is_assumed_state(false);
  return traits;
}

void NiceCover::control(const cover::CoverCall &call) {
  this->target_position_.reset();

  if (call.get_stop()) {
    ESP_LOGD(TAG, "Stop command");
    this->hub_->send_control_cmd(CMD_STOP);
    return;
  }

  if (call.get_position().has_value()) {
    float target = *call.get_position();

    if (target >= cover::COVER_OPEN) {
      ESP_LOGD(TAG, "Open command");
      this->hub_->send_control_cmd(CMD_OPEN);
    } else if (target <= cover::COVER_CLOSED) {
      ESP_LOGD(TAG, "Close command");
      this->hub_->send_control_cmd(CMD_CLOSE);
    } else {
      // Intermediate position: move toward it, on_state_change_ stops the gate there
      float current = this->position;
      if (std::abs(target - current) < POSITION_TOLERANCE) {
        ESP_LOGD(TAG, "Already at %.1f%%", target * 100.0f);
        return;
      }
      this->target_position_ = target;
      if (target > current) {
        ESP_LOGD(TAG, "Opening toward %.1f%%", target * 100.0f);
        this->target_direction_ = cover::COVER_OPERATION_OPENING;
        this->hub_->send_control_cmd(CMD_OPEN);
      } else {
        ESP_LOGD(TAG, "Closing toward %.1f%%", target * 100.0f);
        this->target_direction_ = cover::COVER_OPERATION_CLOSING;
        this->hub_->send_control_cmd(CMD_CLOSE);
      }
    }
  }
}

void NiceCover::on_state_change_() {
  uint8_t state = this->hub_->get_operation_state();
  float pos = this->hub_->get_position_percent();

  cover::CoverOperation op;
  switch (state) {
    case STA_OPENING:
      op = cover::COVER_OPERATION_OPENING;
      break;
    case STA_CLOSING:
      op = cover::COVER_OPERATION_CLOSING;
      break;
    case STA_OPENED:
    case STA_CLOSED:
    case STA_STOPPED:
    case STA_PART_OPENED:
    default:
      op = cover::COVER_OPERATION_IDLE;
      break;
  }

  bool changed = false;
  cover::CoverOperation previous_op = this->last_published_op_;

  if (op != this->last_published_op_) {
    this->last_published_op_ = op;
    this->current_operation = op;
    changed = true;
  }

  // Clamp near-zero to fully closed
  if (pos < CLOSED_THRESHOLD) {
    pos = cover::COVER_CLOSED;
  }

  // No position register and no learned durations: a stop part-way is open
  if (this->hub_->position_unsupported() && state == STA_STOPPED &&
      (pos <= cover::COVER_CLOSED || pos >= cover::COVER_OPEN)) {
    pos = 0.5f;
  }

  // A moving gate is never at an end, so both directions stay available
  if (op == cover::COVER_OPERATION_OPENING || op == cover::COVER_OPERATION_CLOSING) {
    pos = std::max(0.01f, std::min(0.99f, pos));
  }

  // Stop at a requested position once the gate moves the way it was sent; a movement that ends otherwise drops the request.
  // The reported direction can lag or be stale, so the opposite direction is never taken as progress.
  if (this->target_position_.has_value()) {
    float target = *this->target_position_;
    bool movement_ended = op == cover::COVER_OPERATION_IDLE && previous_op != cover::COVER_OPERATION_IDLE;
    bool reached = op == this->target_direction_ &&
                   ((op == cover::COVER_OPERATION_OPENING && pos >= target) ||
                    (op == cover::COVER_OPERATION_CLOSING && pos <= target));
    if (movement_ended) {
      this->target_position_.reset();
    } else if (reached) {
      ESP_LOGD(TAG, "Reached %.1f%%, stopping", target * 100.0f);
      this->target_position_.reset();
      this->hub_->send_control_cmd(CMD_STOP);
    }
  }

  if (pos != this->last_published_pos_) {
    this->last_published_pos_ = pos;
    this->position = pos;
    changed = true;
  }

  if (changed) {
    this->publish_state();
  }
}

}  // namespace nice_bidiwifi
}  // namespace esphome
