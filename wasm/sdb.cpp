#include "sdb.hpp"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <unistd.h>
#include <time.h>
#include <algorithm>

bool read_exact(int sock, void* buf, size_t len) {
    size_t read_bytes = 0;
    while (read_bytes < len) {
        int n = recv(sock, (char*)buf + read_bytes, len - read_bytes, 0);
        if (n <= 0) return false;
        read_bytes += n;
    }
    return true;
}

bool send_adb_msg(int sock, uint32_t cmd, uint32_t arg0, uint32_t arg1, const std::string& payload) {
    adb_msg msg;
    msg.command = cmd;
    msg.arg0 = arg0;
    msg.arg1 = arg1;
    msg.data_length = payload.length();
    msg.data_check = 0;
    for (char c : payload) msg.data_check += (unsigned char)c;
    msg.magic = cmd ^ 0xFFFFFFFF;

    if (send(sock, &msg, sizeof(msg), 0) != sizeof(msg)) return false;
    if (!payload.empty()) {
        if (send(sock, payload.c_str(), payload.length(), 0) != payload.length()) return false;
    }
    return true;
}

bool read_adb_msg(int sock, adb_msg* msg, std::string* payload) {
    if (!read_exact(sock, msg, sizeof(adb_msg))) return false;
    if (msg->data_length > 0) {
        payload->resize(msg->data_length);
        if (!read_exact(sock, &(*payload)[0], msg->data_length)) return false;
    } else {
        payload->clear();
    }
    return true;
}

int connect_to_sdb(std::string& error_msg, int timeout_sec) {
  int sock = socket(AF_INET, SOCK_STREAM, 0);
  if (sock < 0) {
    error_msg = "Failed to create socket for SDB connection";
    return -1;
  }

  struct sockaddr_in serv_addr;
  serv_addr.sin_family = AF_INET;
  serv_addr.sin_port = htons(26101);
  inet_pton(AF_INET, "127.0.0.1", &serv_addr.sin_addr);

  struct timeval tv;
  tv.tv_sec = timeout_sec;
  tv.tv_usec = 0;
  setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof tv);
  setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof tv);

  if (connect(sock, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
    close(sock);
    error_msg = "Failed to connect to local SDB daemon on port 26101";
    return -1;
  }

  if (!send_adb_msg(sock, 0x4e584e43 /*CNXN*/, 0x01000000, 4096, "host::moonlight")) {
    close(sock);
    error_msg = "Failed to send SDB handshake";
    return -1;
  }

  adb_msg msg;
  std::string payload;
  while (read_adb_msg(sock, &msg, &payload)) {
      if (msg.command == 0x4e584e43 /*CNXN*/) {
          return sock; // Handshake successful
      }
      if (msg.command == 0x48545541 /*AUTH*/) {
          close(sock);
          error_msg = "SDB daemon requires RSA authentication. Please use Tizen Studio to deploy instead.";
          return -1;
      }
  }

  close(sock);
  error_msg = "SDB daemon rejected handshake.";
  return -1;
}

std::string sync_push_file(int sock, uint32_t local_id, const std::vector<unsigned char>& data_buf, const std::string& remote_path) {
    std::string open_payload = "sync:";
    open_payload.push_back('\0');
    if (!send_adb_msg(sock, 0x4e45504f /*OPEN*/, local_id, 0, open_payload)) return "Failed to send OPEN sync:";

    adb_msg msg; std::string payload; uint32_t remote_id = 0;
    while (read_adb_msg(sock, &msg, &payload)) {
        if (msg.command == 0x59414b4f /*OKAY*/) { remote_id = msg.arg0; break; }
        if (msg.command == 0x45534c43 /*CLSE*/) return "Channel closed while waiting for OKAY after OPEN";
    }

    auto send_sync_chunk = [&](const char* data, size_t len) -> std::string {
        size_t offset = 0;
        while (offset < len) {
            size_t chunk = std::min((size_t)4096, len - offset);
            std::string s(data + offset, chunk);
            if (!send_adb_msg(sock, 0x45545257 /*WRTE*/, local_id, remote_id, s)) return "Failed to send WRTE chunk";
            adb_msg resp; std::string rpayload;
            while (read_adb_msg(sock, &resp, &rpayload)) {
                if (resp.command == 0x59414b4f /*OKAY*/) break;
                if (resp.command == 0x45534c43 /*CLSE*/) return "Channel closed while waiting for chunk OKAY";
            }
            offset += chunk;
        }
        return "";
    };

    std::string send_req;
    send_req.append("SEND", 4);
    std::string path_mode = remote_path + ",33277";
    uint32_t length = path_mode.length();
    send_req.append((char*)&length, 4);
    send_req.append(path_mode);
    std::string err = send_sync_chunk(send_req.data(), send_req.length());
    if (!err.empty()) return "SEND request failed: " + err;

    size_t data_offset = 0;
    while (data_offset < data_buf.size()) {
        std::string chunk;
        chunk.append("DATA", 4);
        uint32_t chunk_len = std::min((size_t)3000, data_buf.size() - data_offset);
        chunk.append((char*)&chunk_len, 4);
        chunk.append((const char*)&data_buf[data_offset], chunk_len);
        err = send_sync_chunk(chunk.data(), chunk.length());
        if (!err.empty()) { return "DATA chunk failed: " + err; }
        data_offset += chunk_len;
    }

    std::string done_req;
    done_req.append("DONE", 4);
    uint32_t mtime = time(NULL);
    done_req.append((char*)&mtime, 4);
    err = send_sync_chunk(done_req.data(), done_req.length());
    if (!err.empty()) return "DONE request failed: " + err;

    while (read_adb_msg(sock, &msg, &payload)) {
        if (msg.command == 0x45545257 /*WRTE*/) {
            send_adb_msg(sock, 0x59414b4f /*OKAY*/, local_id, remote_id, "");
            if (payload.length() >= 4 && payload.substr(0, 4) == "OKAY") break;
            if (payload.length() >= 4 && payload.substr(0, 4) == "FAIL") {
                return "Sync FAIL from daemon: " + payload.substr(8);
            }
        }
        if (msg.command == 0x45534c43 /*CLSE*/) return "Channel closed while waiting for final SYNC OKAY";
    }

    std::string quit_req;
    quit_req.append("QUIT", 4);
    uint32_t zero = 0;
    quit_req.append((char*)&zero, 4);
    send_sync_chunk(quit_req.data(), quit_req.length());

    send_adb_msg(sock, 0x45534c43 /*CLSE*/, local_id, remote_id, "");
    return "";
}
