-- A Bunty utterance uses a fresh WebSocket, so the gateway cannot use a
-- connection-local counter to order conversation history. Keep a durable,
-- per-session sequence and allocate its next value atomically on each write.

CREATE TABLE IF NOT EXISTS conversation_sessions (
  device_id TEXT NOT NULL,
  session_id TEXT NOT NULL,
  next_turn_index INTEGER NOT NULL DEFAULT 0,
  PRIMARY KEY (device_id, session_id)
);

-- Preserve the latest known index for sessions written before this sequence
-- existed. Those deployments only retained index 1, so the next turn becomes
-- 2 instead of replacing it.
INSERT INTO conversation_sessions (device_id, session_id, next_turn_index)
SELECT device_id, session_id, MAX(turn_index)
  FROM conversation_turns
 GROUP BY device_id, session_id
ON CONFLICT (device_id, session_id)
DO UPDATE SET next_turn_index = MAX(
  conversation_sessions.next_turn_index,
  excluded.next_turn_index
);
