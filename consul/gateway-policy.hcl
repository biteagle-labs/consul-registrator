# Consul ACL policy for the OpenResty gateway.
#
# The gateway needs three reads:
#   1. its own agent's node name   (GET /v1/agent/self -> Config.NodeName)
#   2. the local agent's services  (GET /v1/agent/services)
#   3. its per-node routing config (GET /v1/kv/gateway/<node>/configuration)
#
# Apply:
#   consul acl policy create -name gateway -rules @consul/gateway-policy.hcl
#   consul acl token  create -description gateway -policy-name gateway
# Then pass the token to the gateway container as CONSUL_TOKEN.

# Learn this node's name from the local agent (/v1/agent/self).
agent_prefix "" {
  policy = "read"
}

# Discover services registered on the local agent.
service_prefix "" {
  policy = "read"
}

# Read the per-node routing config published under gateway/.
key_prefix "gateway/" {
  policy = "read"
}
