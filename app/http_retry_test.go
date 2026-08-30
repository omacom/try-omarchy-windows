package main

import (
	"fmt"
	"net"
	"net/http"
	"net/http/httptest"
	"sync/atomic"
	"testing"
)

func TestGetWithSetupRetryRecoversFromTemporaryFailure(t *testing.T) {
	configureSetupCancellation(false)
	var requests atomic.Int32
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
		if requests.Add(1) < 3 {
			http.Error(w, "temporary", http.StatusServiceUnavailable)
			return
		}
		_, _ = fmt.Fprint(w, "ok")
	}))
	defer server.Close()
	response, err := getWithSetupRetry(server.Client(), server.URL, 3)
	if err != nil {
		t.Fatal(err)
	}
	response.Body.Close()
	if requests.Load() != 3 {
		t.Fatalf("requests = %d", requests.Load())
	}
}

func TestSetupFailureHelpExplainsDNS(t *testing.T) {
	err := fmt.Errorf("download failed: %w", &net.DNSError{Err: "no such host", Name: "github.com"})
	if got := setupFailureHelp(err); got == "Check your connection and start Try Omarchy again." {
		t.Fatalf("DNS error got generic help: %q", got)
	}
}

func TestGetWithSetupRetryStopsAfterBound(t *testing.T) {
	configureSetupCancellation(false)
	client := &http.Client{Transport: roundTripFunc(func(*http.Request) (*http.Response, error) {
		return nil, fmt.Errorf("lookup github.com: no such host")
	})}
	if _, err := getWithSetupRetry(client, "https://github.com/file", 2); err == nil {
		t.Fatal("persistent DNS failure was accepted")
	}
}
