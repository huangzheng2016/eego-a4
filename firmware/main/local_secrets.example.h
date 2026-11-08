// SPDX-License-Identifier: GPL-3.0-or-later
//
//   cp firmware/main/local_secrets.example.h firmware/main/local_secrets.h
//
//
#pragma once

#define EEGO_LOCAL_WIFI_LIST                    \
  {                                             \
    {"your-home-ssid", "your-home-password"},   \
    {"your-office-ssid", "your-office-password"}, \
    {"your-phone-hotspot", ""},                 \
  }

// #define EEGO_LOCAL_WIFI_SSID "your-ssid"

// #define EEGO_LOCAL_WIFI_REPLACE

#define EEGO_LOCAL_ADMIN_PASSWORD "your-admin-password"

// #define EEGO_LOCAL_POWER_MODE_MAINS
// #define EEGO_LOCAL_TIMEZONE_OFFSET_MIN 480
