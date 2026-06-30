#include <tizen.h>
#include <service_app.h>
#include <dlog.h>
#include <message_port.h>
#include <pthread.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <arpa/inet.h>

#include "../mdns/mdns.h"

#define LOG_TAG "MoonlightMdnsService"
#define UI_APP_ID "MoonLightS.MoonlightWasm"
#define LOCAL_PORT "MdnsRequestPort"
#define REMOTE_PORT "MdnsResponsePort"

static int g_running = 0;
static pthread_t g_thread;
static int g_local_port_id = -1;

// Callback for mjansson/mdns when a record is found
static int query_callback(int sock, const struct sockaddr* from, size_t addrlen,
                          mdns_entry_type_t entry, uint16_t query_id,
                          uint16_t rtype, uint16_t rclass, uint32_t ttl,
                          const void* data, size_t size, size_t name_offset,
                          size_t name_length, size_t record_offset,
                          size_t record_length, void* user_data) {
    (void)sizeof(sock);
    (void)sizeof(query_id);
    (void)sizeof(name_length);
    (void)sizeof(user_data);

    if (rtype == MDNS_RECORDTYPE_A && from->sa_family == AF_INET) {
        struct sockaddr_in* addr = (struct sockaddr_in*)from;
        char ipstr[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &(addr->sin_addr), ipstr, INET_ADDRSTRLEN);

        dlog_print(DLOG_INFO, LOG_TAG, "mDNS Response: Found Sunshine at %s", ipstr);

        // Send this IP back to the UI via Message Port
        bundle *b = bundle_create();
        bundle_add_str(b, "ip", ipstr);
        
        int ret = message_port_send_message(UI_APP_ID, REMOTE_PORT, b);
        if (ret != MESSAGE_PORT_ERROR_NONE) {
            dlog_print(DLOG_ERROR, LOG_TAG, "Failed to send message to UI: %d", ret);
        }
        bundle_free(b);
    }
    return 0;
}

// mDNS Discovery Thread
static void* mdns_thread(void* arg) {
    (void)arg;
    dlog_print(DLOG_INFO, LOG_TAG, "Starting mDNS Discovery thread...");

    size_t capacity = 2048;
    void* buffer = malloc(capacity);

    int sock = mdns_socket_open_ipv4(NULL);
    if (sock < 0) {
        dlog_print(DLOG_ERROR, LOG_TAG, "Failed to open IPv4 socket for mDNS");
        free(buffer);
        return NULL;
    }

    dlog_print(DLOG_INFO, LOG_TAG, "Sending mDNS query for _nvstream._tcp.local.");
    int res = mdns_query_send(sock, MDNS_RECORDTYPE_PTR,
                              "_nvstream._tcp.local.", 21,
                              buffer, capacity, 0);

    if (res < 0) {
        dlog_print(DLOG_ERROR, LOG_TAG, "Failed to send mDNS query");
    } else {
        // Listen for responses for up to 3 seconds
        dlog_print(DLOG_INFO, LOG_TAG, "Reading mDNS responses...");
        int i;
        for (i = 0; i < 30 && g_running; i++) {
            res = mdns_query_recv(sock, buffer, capacity, query_callback, NULL, 0);
            if (res > 0) {
                dlog_print(DLOG_INFO, LOG_TAG, "Processed %d mDNS records", res);
            }
            usleep(100000); // 100ms
        }
    }

    mdns_socket_close(sock);
    free(buffer);
    dlog_print(DLOG_INFO, LOG_TAG, "mDNS Discovery thread finished.");
    return NULL;
}

// Callback when the UI sends a message to this service
static void on_message_received(int local_port_id, const char *remote_app_id, const char *remote_port,
                                bool trusted_remote_port, bundle *message, void *user_data) {
    (void)local_port_id;
    (void)remote_app_id;
    (void)remote_port;
    (void)trusted_remote_port;
    (void)user_data;

    char *cmd = NULL;
    bundle_get_str(message, "cmd", &cmd);

    if (cmd && strcmp(cmd, "search") == 0) {
        dlog_print(DLOG_INFO, LOG_TAG, "Received 'search' command from UI");
        
        // Start mDNS thread
        if (g_running) {
            // A search is already running, wait for it to finish or just start a new one?
            // For simplicity, we just join the old thread if it exists
            g_running = 0;
            pthread_join(g_thread, NULL);
        }
        g_running = 1;
        pthread_create(&g_thread, NULL, mdns_thread, NULL);
    }
}

static bool service_app_create(void *data) {
    (void)data;
    dlog_print(DLOG_INFO, LOG_TAG, "Service created");
    
    // Register the local message port to listen to UI requests
    int port_id = message_port_register_local_port(LOCAL_PORT, on_message_received, NULL);
    if (port_id < 0) {
        dlog_print(DLOG_ERROR, LOG_TAG, "Failed to register local port: %d", port_id);
        return false;
    }
    g_local_port_id = port_id;
    dlog_print(DLOG_INFO, LOG_TAG, "Local port registered: %s (id=%d)", LOCAL_PORT, port_id);
    
    return true;
}

static void service_app_terminate(void *data) {
    (void)data;
    dlog_print(DLOG_INFO, LOG_TAG, "Service terminated");
    g_running = 0;
    if (g_local_port_id >= 0) {
        message_port_unregister_local_port(g_local_port_id);
        g_local_port_id = -1;
    }
}

static void service_app_control(app_control_h app_control, void *data) {
    (void)app_control;
    (void)data;
    dlog_print(DLOG_INFO, LOG_TAG, "Service app control called");
}

int main(int argc, char* argv[]) {
    service_app_lifecycle_callback_s event_callback;
    memset(&event_callback, 0, sizeof(event_callback));

    event_callback.create = service_app_create;
    event_callback.terminate = service_app_terminate;
    event_callback.app_control = service_app_control;

    return service_app_main(argc, argv, &event_callback, NULL);
}
