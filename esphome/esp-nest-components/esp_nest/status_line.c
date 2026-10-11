#include "status_line.h"

#include <stdio.h>

int status_line_format(char *buf, size_t size, bool wifi, uint32_t ip_v4,
                       bool mqtt, const char *version)
{
    if (!wifi)
        return snprintf(buf, size, "WiFi: disconnected | v%s", version);

    return snprintf(buf, size, "IP: %u.%u.%u.%u | MQTT: %s | v%s",
                    (unsigned)(ip_v4 >> 24 & 0xFF), (unsigned)(ip_v4 >> 16 & 0xFF),
                    (unsigned)(ip_v4 >> 8 & 0xFF), (unsigned)(ip_v4 & 0xFF),
                    mqtt ? "ok" : "offline", version);
}
