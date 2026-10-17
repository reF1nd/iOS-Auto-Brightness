// SPDX-License-Identifier: GPL-3.0-only
package org.ref1nd.brightness;

/** Separates a pending output probe from positive evidence of unsafe output. */
public final class OutputProbeGuard {
    public enum Result { SAFE, UNSAFE, UNKNOWN }
    public enum State { WAIT, SAFE, UNSAFE, FAULT }
    public static final long INTERVAL_MS=1000, FRESH_MS=6000;

    public static final class Ticket {
        public final long generation, startedAt;
        private Ticket(long generation,long startedAt) {this.generation=generation;this.startedAt=startedAt;}
    }

    private long generation=0, requestedAt=-1, completedAt=-1, trustedAt=-1, activeAt=-1, observedAt=-1;
    private Ticket pending;
    private Result result=Result.UNKNOWN;
    private boolean fault=false;

    /** Invalidate on a display/wake-session boundary, not on every off-screen tick. */
    public synchronized void invalidate() {
        generation++;requestedAt=completedAt=trustedAt=activeAt=observedAt=-1;
        pending=null;result=Result.UNKNOWN;
        // A fault requires explicit service recovery; waking cannot clear it.
    }

    /** null means a probe is already running, the interval has not elapsed, or faulted. */
    public synchronized Ticket begin(long now) {
        if(now<0)throw new IllegalArgumentException("negative probe time");
        expire(now);
        if(fault||pending!=null||(requestedAt>=0&&now-requestedAt<INTERVAL_MS))return null;
        requestedAt=now;pending=new Ticket(generation,now);return pending;
    }

    /** Use the completion timestamp, never the request timestamp, for freshness. */
    public synchronized boolean complete(Ticket ticket,long now,Result value) {
        if(value==null)throw new IllegalArgumentException("missing probe result");
        if(ticket==null||ticket!=pending||ticket.generation!=generation||now<ticket.startedAt)return false;
        expire(now);
        pending=null;
        if(fault)return false;
        completedAt=now;result=value;
        if(value!=Result.UNKNOWN)trustedAt=now;
        return true;
    }

    public synchronized State state(long now,boolean active) {
        if(now<0)throw new IllegalArgumentException("negative probe time");
        if(active&&activeAt<0)activeAt=now;
        expire(now);
        if(fault)return State.FAULT;
        if(completedAt<0||now<completedAt||now-completedAt>FRESH_MS)return State.WAIT;
        if(result==Result.UNSAFE)return State.UNSAFE;
        if(result==Result.SAFE)return State.SAFE;
        return State.WAIT;
    }

    private void expire(long now) {
        // Once control was active, dropping active=false cannot evade the timeout.
        // UNKNOWN responses do not renew this deadline. SAFE/UNSAFE are both
        // credible observations; the caller decides how to handle explicit UNSAFE.
        // A worker completion can be delivered after a newer main-thread state
        // check. Preserve its real timestamp without moving the liveness clock back.
        observedAt=Math.max(observedAt,now);
        long since=Math.max(activeAt,trustedAt);
        if(activeAt>=0&&observedAt-since>FRESH_MS)fault=true;
    }

    public synchronized long generation(){return generation;}
    public synchronized long completedAt(){return completedAt;}
    public synchronized long lastCompletedAt(){return completedAt;}
    public synchronized long lastStartedAt(){return requestedAt;}
    public synchronized Result lastResult(){return result;}
    public synchronized boolean pending(){return pending!=null;}
}
