package main

import (
	"errors"
	"strings"
	"testing"
)

func mustForward(t *testing.T, value string) portForward {
	t.Helper()
	f, err := parseForward(value)
	if err != nil {
		t.Fatal(err)
	}
	return f
}

func TestPlanLiveForwardsDefersLANAndSSH(t *testing.T) {
	local8080 := mustForward(t, "tcp:8080:80")
	local9000 := mustForward(t, "tcp:9000:9000")
	ssh := mustForward(t, "tcp:2222:22")
	lan := mustForward(t, "tcp:192.168.1.5:8443:443")
	plan := planLiveForwards([]portForward{local8080, ssh}, []portForward{local9000, lan})
	if len(plan.remove) != 1 || plan.remove[0] != local8080 {
		t.Fatalf("remove = %v", plan.remove)
	}
	if len(plan.add) != 1 || plan.add[0] != local9000 {
		t.Fatalf("add = %v", plan.add)
	}
	if len(plan.deferred) != 2 || plan.deferred[0] != ssh || plan.deferred[1] != lan {
		t.Fatalf("deferred = %v", plan.deferred)
	}
	if plan := planLiveForwards([]portForward{local8080}, []portForward{local8080}); len(plan.add)+len(plan.remove)+len(plan.deferred) != 0 {
		t.Fatalf("unchanged list produced %+v", plan)
	}
}

func TestApplyLiveForwardsReadsMonitorReplies(t *testing.T) {
	old := mustForward(t, "tcp:8080:80")
	fresh := mustForward(t, "udp:5353:53")
	busy := mustForward(t, "tcp:9000:9000")
	var commands []string
	monitor := func(line string) (string, error) {
		commands = append(commands, line)
		switch {
		case strings.HasPrefix(line, "hostfwd_remove"):
			return "host forwarding rule for tcp:127.0.0.1:8080 removed\r\n", nil
		case strings.Contains(line, ":9000-"):
			return "Could not set up host forwarding rule 'tcp:127.0.0.1:9000-:9000'\r\n", nil
		}
		return "", nil
	}
	active, errs := applyLiveForwards([]portForward{old}, forwardPlan{remove: []portForward{old}, add: []portForward{fresh, busy}}, monitor)
	want := []string{"hostfwd_remove n0 tcp:127.0.0.1:8080", "hostfwd_add n0 udp:127.0.0.1:5353-:53", "hostfwd_add n0 tcp:127.0.0.1:9000-:9000"}
	if strings.Join(commands, "|") != strings.Join(want, "|") {
		t.Fatalf("commands = %q", commands)
	}
	if len(active) != 1 || active[0] != fresh {
		t.Fatalf("active = %v", active)
	}
	if len(errs) != 1 || !strings.Contains(errs[0].Error(), "Could not set up") {
		t.Fatalf("errs = %v", errs)
	}
	active, errs = applyLiveForwards(active, forwardPlan{remove: []portForward{fresh}}, func(string) (string, error) { return "", errors.New("monitor gone") })
	if len(active) != 1 || len(errs) != 1 {
		t.Fatalf("a failed removal changed the active list: %v %v", active, errs)
	}
}

func TestForwardsForBootFollowsLiveChanges(t *testing.T) {
	launched := []portForward{mustForward(t, "tcp:8080:80"), mustForward(t, "tcp:2222:22")}
	liveForwardState.Lock()
	liveForwardState.set, liveForwardState.active = false, nil
	liveForwardState.Unlock()
	if got := forwardsForBoot(launched); len(got) != 2 {
		t.Fatalf("before any live change got %v", got)
	}
	setLiveForwards([]portForward{mustForward(t, "tcp:2222:22")})
	got := forwardsForBoot(launched)
	if len(got) != 1 || got[0] != mustForward(t, "tcp:2222:22") {
		t.Fatalf("a reboot after removing a forward would still get %v", got)
	}
	liveForwardState.Lock()
	liveForwardState.set, liveForwardState.active = false, nil
	liveForwardState.Unlock()
}

func TestPlanLiveForwardsTreatsExplicitLoopbackAsTheSameForward(t *testing.T) {
	plan := planLiveForwards([]portForward{mustForward(t, "tcp:8080:80")}, []portForward{mustForward(t, "tcp:127.0.0.1:8080:80")})
	if len(plan.add)+len(plan.remove)+len(plan.deferred) != 0 {
		t.Fatalf("rewriting the same forward changed it: %+v", plan)
	}
}
