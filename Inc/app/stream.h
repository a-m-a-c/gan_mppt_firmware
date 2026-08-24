#ifndef STREAM_H
#define STREAM_H

/* Periodic one-way telemetry over serial. The packet set is fixed at compile
   time - nothing here is host-configurable. */

void stream_init(void);

// Run every loop. Sends the current packet set every STREAM_PERIOD_MS.
void stream_service(void);

#endif /* STREAM_H */
