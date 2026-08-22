/* SPDX-License-Identifier: LicenseRef-Source-Available-No-Redistribution */
#include "espos_n2k/candump_format.h"

#include <cstdio>
#include <cstdlib>   // strtoll, strtoul
#include <cstring>
#include <sys/time.h>

namespace espos_n2k {

int candump_encode(const CanMessage& msg, const char* iface,
                   char* buf, size_t buf_len) {
  // Format: (seconds.microseconds) iface CANID#HEXDATA\n
  // Use wall-clock time (Unix epoch) so SignalK gets valid timestamps.
  // Falls back to uptime if NTP hasn't synced yet (time < 2020).
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  int64_t sec, usec;
  if (tv.tv_sec > 1577836800) {  // after 2020-01-01
    sec = tv.tv_sec;
    usec = tv.tv_usec;
  } else {
    sec = msg.timestamp_us / 1000000;
    usec = msg.timestamp_us % 1000000;
  }

  // Build the hex data string
  char data_hex[17];  // max 8 bytes = 16 hex chars + null
  int data_len = msg.frame.dlc;
  if (data_len > (int)kCanMaxData) data_len = (int)kCanMaxData;
  for (int i = 0; i < data_len; i++) {
    snprintf(data_hex + i * 2, 3, "%02X", msg.frame.data[i]);
  }
  data_hex[data_len * 2] = '\0';

  // CAN ID — always 8 hex digits for extended frames (NMEA 2000)
  int n = snprintf(buf, buf_len, "(%lld.%06lld) %s %08X#%s\n",
                   (long long)sec, (long long)usec,
                   iface, (unsigned)msg.frame.id, data_hex);
  if (n < 0 || (size_t)n >= buf_len) return -1;
  return n;
}

bool candump_decode(const char* line, CanMessage* out) {
  // Parse: (seconds.microseconds) iface CANID#HEXDATA
  if (!line || !out) return false;

  // Skip leading whitespace
  while (*line == ' ' || *line == '\t') line++;

  // Parse timestamp: (sec.usec)
  int64_t sec = 0, usec = 0;
  if (*line == '(') {
    line++;
    char* end;
    sec = strtoll(line, &end, 10);
    if (*end == '.') {
      end++;
      usec = strtoll(end, &end, 10);
    }
    if (*end == ')') end++;
    line = end;
  }
  out->timestamp_us = sec * 1000000 + usec;

  // Skip whitespace + interface name
  while (*line == ' ') line++;
  while (*line && *line != ' ') line++;  // skip iface
  while (*line == ' ') line++;

  // Parse CAN ID (hex, up to 8 digits)
  char* hash;
  unsigned long can_id = strtoul(line, &hash, 16);
  if (*hash != '#') return false;
  hash++;

  out->frame.id = can_id;
  out->frame.extended = true;  // NMEA 2000 always extended
  out->frame.remote = false;

  // Parse hex data bytes
  int data_len = 0;
  while (*hash && *hash != '\n' && *hash != '\r' && data_len < (int)kCanMaxData) {
    unsigned int byte;
    if (sscanf(hash, "%2x", &byte) != 1) break;
    out->frame.data[data_len++] = (uint8_t)byte;
    hash += 2;
  }
  out->frame.dlc = (uint8_t)data_len;

  return data_len > 0;
}

}  // namespace espos_n2k
