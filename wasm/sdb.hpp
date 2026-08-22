#pragma once

#include <string>
#include <vector>
#include <stdint.h>

struct adb_msg {
    uint32_t command;
    uint32_t arg0;
    uint32_t arg1;
    uint32_t data_length;
    uint32_t data_check;
    uint32_t magic;
};

bool read_exact(int sock, void* buf, size_t len);
bool send_adb_msg(int sock, uint32_t cmd, uint32_t arg0, uint32_t arg1, const std::string& payload);
bool read_adb_msg(int sock, adb_msg* msg, std::string* payload);
int connect_to_sdb(std::string& error_msg, int timeout_sec);
std::string sync_push_file(int sock, uint32_t local_id, const std::vector<unsigned char>& data_buf, const std::string& remote_path);
