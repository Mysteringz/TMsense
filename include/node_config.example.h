#ifndef NODE_CONFIG_H
#define NODE_CONFIG_H

// Optional bench defaults. Copy to node_config.h (git-ignored) and edit.
// Settings saved on the node (`set ... ` + `save` over serial) override these.
// Never put real credentials in this template.

#define TM_DEFAULT_SSID     "your-2.4GHz-ssid"
#define TM_DEFAULT_PASSWORD "your-password"

// The TMedge machine(s), comma-separated dotted quads.
#define TM_DEFAULT_EDGES    "192.168.0.100"

// Shared with TMedge (its TM_KEY environment variable). Generate with
//   openssl rand -hex 32
// Empty = unsigned telemetry, which TMedge refuses unless ALLOW_UNSIGNED=1.
#define TM_DEFAULT_KEY      ""

#endif
