// SPDX-License-Identifier: GPL-3.0-only
package org.ref1nd.brightness;

/** Runtime health, distinct from the existence of a reflection method. */
public final class EventCapability {
    private final boolean supported;
    private boolean healthy=false;
    private long retryAt=0;
    public String status;
    public EventCapability(boolean supported){this.supported=supported;status=supported?"probing":"missing_interface";}
    public boolean available(){return healthy;}
    public boolean probe(long now){return supported&&now>=retryAt;}
    public void success(){healthy=true;retryAt=0;status="available";}
    public void failure(long now,String error){healthy=false;retryAt=now+30000;status="error: "+error;}
}
