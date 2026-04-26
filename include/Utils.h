#pragma once

// ログレベル制御
// ビルドフラグで -DSAMPLER_LOG_LEVEL=0 (エラーのみ) / 1 (INFO以上) / 2 (DEBUG以上) を指定可能
#ifndef SAMPLER_LOG_LEVEL
#define SAMPLER_LOG_LEVEL 1 // デフォルト: INFO有効
#endif

#if defined(ESP_PLATFORM) && !defined(ARDUINO)
#include <esp_log.h>
#if SAMPLER_LOG_LEVEL >= 2
#define LOGD(tag, format, ...) ESP_LOGD(tag, format, ##__VA_ARGS__)
#else
#define LOGD(tag, format, ...) ((void)0)
#endif
#if SAMPLER_LOG_LEVEL >= 1
#define LOGI(tag, format, ...) ESP_LOGI(tag, format, ##__VA_ARGS__)
#else
#define LOGI(tag, format, ...) ((void)0)
#endif
#define LOGE(tag, format, ...) ESP_LOGE(tag, format, ##__VA_ARGS__)
#elif defined(ARDUINO)
#include <Arduino.h>
#if SAMPLER_LOG_LEVEL >= 2
#define LOGD(tag, format, ...) Serial.printf("[%s] " format "\n", tag, ##__VA_ARGS__)
#else
#define LOGD(tag, format, ...) ((void)0)
#endif
#if SAMPLER_LOG_LEVEL >= 1
#define LOGI(tag, format, ...) Serial.printf("[%s] " format "\n", tag, ##__VA_ARGS__)
#else
#define LOGI(tag, format, ...) ((void)0)
#endif
#define LOGE(tag, format, ...) Serial.printf("[%s] " format "\n", tag, ##__VA_ARGS__)
#else
#include <cstdio>
#if SAMPLER_LOG_LEVEL >= 2
#define LOGD(tag, format, ...) printf("[%s] " format "\n", tag, ##__VA_ARGS__)
#else
#define LOGD(tag, format, ...) ((void)0)
#endif
#if SAMPLER_LOG_LEVEL >= 1
#define LOGI(tag, format, ...) printf("[%s] " format "\n", tag, ##__VA_ARGS__)
#else
#define LOGI(tag, format, ...) ((void)0)
#endif
#define LOGE(tag, format, ...) printf("[%s] " format "\n", tag, ##__VA_ARGS__)
#endif

namespace capsule
{
namespace sampler
{

// ArduinoのmicrosをArduino以外の環境でも使用するための関数
unsigned long micros();

}
}
