package main

import (
	"errors"
	"fmt"
	"net"
	"net/http"
	"time"
)

func getWithSetupRetry(client *http.Client, source string, attempts int) (*http.Response, error) {
	if attempts < 1 {
		attempts = 1
	}
	var lastErr error
	for attempt := 0; attempt < attempts; attempt++ {
		req, err := http.NewRequestWithContext(setupContext(), http.MethodGet, source, nil)
		if err != nil {
			return nil, err
		}
		resp, err := client.Do(req)
		if err == nil && !retryableHTTPStatus(resp.StatusCode) {
			return resp, nil
		}
		if err == nil {
			lastErr = fmt.Errorf("HTTP %d", resp.StatusCode)
			resp.Body.Close()
		} else {
			lastErr = err
		}
		if attempt+1 == attempts {
			break
		}
		delay := time.Second << min(attempt, 3)
		if err := sleepDuringSetup(delay); err != nil {
			return nil, err
		}
	}
	return nil, lastErr
}

func retryableHTTPStatus(status int) bool {
	return status == http.StatusRequestTimeout || status == http.StatusTooManyRequests || status >= 500
}

func setupFailureHelp(err error) string {
	var dnsErr *net.DNSError
	if errors.As(err, &dnsErr) {
		return "Windows could not resolve github.com. Check that this PC or VM has internet access, then start Try Omarchy again."
	}
	return "Check your connection and start Try Omarchy again."
}
