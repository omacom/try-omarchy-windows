package main

import (
	"context"
	"crypto/rand"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net"
	"net/http"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"time"
)

const transferTicketLifetime = 30 * time.Minute

type fileTransferTicket struct {
	ID        string            `json:"id"`
	Token     string            `json:"token"`
	Direction string            `json:"direction"`
	Offer     fileTransferOffer `json:"offer"`
	// Point is the guest display coordinate a direct drop was released at, set
	// only when the files were dropped onto the running VM window.
	Point []int `json:"point,omitempty"`
}

type fileTransferStatus struct {
	State string `json:"state"`
	Bytes int64  `json:"bytes"`
	Total int64  `json:"total"`
	Phase string `json:"phase"`
}

type fileTransferJob struct {
	ticket          fileTransferTicket
	status          fileTransferStatus
	expires         time.Time
	ctx             context.Context
	cancel          context.CancelFunc
	archive         string
	file            *os.File
	destination     string
	attempts        int
	activeDownloads int
}

// Adapters register only user-selected sources or an accepted destination.
// Network requests carry capability tokens, never arbitrary host paths.
type fileTransferService struct {
	mu        sync.Mutex
	jobs      map[string]*fileTransferJob
	cache     string
	limits    fileTransferLimits
	ctx       context.Context
	cancel    context.CancelFunc
	ioSlot    chan struct{}
	downloads chan struct{}
	now       func() time.Time
}

func newFileTransferService(cache string, limits fileTransferLimits) *fileTransferService {
	ctx, cancel := context.WithCancel(context.Background())
	return &fileTransferService{jobs: map[string]*fileTransferJob{}, cache: cache, limits: limits, ctx: ctx, cancel: cancel, ioSlot: make(chan struct{}, 1), downloads: make(chan struct{}, 2), now: time.Now}
}

func randomTransferToken(bytes int) string {
	data := make([]byte, bytes)
	rand.Read(data)
	return hex.EncodeToString(data)
}

func (s *fileTransferService) register(direction string, offer fileTransferOffer) (*fileTransferJob, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	if err := s.ctx.Err(); err != nil {
		return nil, err
	}
	for token, job := range s.jobs {
		if job.activeDownloads == 0 && job.status.State != "transferring" && !s.now().Before(job.expires) {
			s.release(job)
			delete(s.jobs, token)
		}
	}
	if len(s.jobs) >= 32 {
		return nil, fmt.Errorf("finish or cancel another file transfer first")
	}
	ctx, cancel := context.WithCancel(s.ctx)
	job := &fileTransferJob{ticket: fileTransferTicket{ID: randomTransferToken(16), Token: randomTransferToken(32), Direction: direction, Offer: offer}, status: fileTransferStatus{State: "ready"}, expires: s.now().Add(transferTicketLifetime), ctx: ctx, cancel: cancel}
	s.jobs[job.ticket.Token] = job
	return job, nil
}

func (s *fileTransferService) release(job *fileTransferJob) {
	job.cancel()
	if job.file != nil {
		job.file.Close()
	}
	if job.archive != "" {
		os.Remove(job.archive)
	}
}

func (s *fileTransferService) Close() {
	s.cancel()
	s.mu.Lock()
	defer s.mu.Unlock()
	for _, job := range s.jobs {
		s.release(job)
	}
	s.jobs = map[string]*fileTransferJob{}
}

func (s *fileTransferService) Cancel(id string) bool {
	s.mu.Lock()
	defer s.mu.Unlock()
	for token, job := range s.jobs {
		if job.ticket.ID == id {
			s.release(job)
			delete(s.jobs, token)
			return true
		}
	}
	return false
}

func (s *fileTransferService) Status(id string) (fileTransferStatus, bool) {
	s.mu.Lock()
	defer s.mu.Unlock()
	for _, job := range s.jobs {
		if job.ticket.ID == id {
			return job.status, true
		}
	}
	return fileTransferStatus{}, false
}

func (s *fileTransferService) acquire(ctx context.Context) error {
	select {
	case s.ioSlot <- struct{}{}:
		return nil
	case <-ctx.Done():
		return ctx.Err()
	case <-s.ctx.Done():
		return s.ctx.Err()
	}
}

func (s *fileTransferService) Offer(ctx context.Context, sources []string, progress backupProgress) (fileTransferTicket, error) {
	ctx, cancel := context.WithCancel(ctx)
	defer cancel()
	stop := context.AfterFunc(s.ctx, cancel)
	defer stop()
	if err := s.acquire(ctx); err != nil {
		return fileTransferTicket{}, err
	}
	defer func() { <-s.ioSlot }()
	offer, archive, err := prepareFileTransfer(ctx, sources, s.cache, s.limits, progress)
	if err != nil {
		return fileTransferTicket{}, err
	}
	file, err := openSavedMemory(archive)
	if err != nil {
		os.Remove(archive)
		return fileTransferTicket{}, err
	}
	identity, err := hashSavedMemory(ctx, file)
	if err != nil || identity.Bytes != offer.ArchiveBytes || identity.SHA256 != offer.SHA256 {
		file.Close()
		os.Remove(archive)
		if err == nil {
			err = fmt.Errorf("prepared transfer changed before registration")
		}
		return fileTransferTicket{}, err
	}
	job, err := s.register("download", offer)
	if err != nil {
		file.Close()
		os.Remove(archive)
		return fileTransferTicket{}, err
	}
	s.mu.Lock()
	if ctx.Err() != nil || s.ctx.Err() != nil || job.ctx.Err() != nil {
		delete(s.jobs, job.ticket.Token)
		job.cancel()
		s.mu.Unlock()
		file.Close()
		os.Remove(archive)
		return fileTransferTicket{}, context.Canceled
	}
	job.archive = archive
	job.file = file
	s.mu.Unlock()
	return job.ticket, nil
}

// AcceptReceive is called by a local adapter after destination acceptance.
func (s *fileTransferService) AcceptReceive(offer fileTransferOffer, destination string) (fileTransferTicket, error) {
	if !filepath.IsAbs(destination) || !offer.valid(s.limits) {
		return fileTransferTicket{}, fmt.Errorf("invalid transfer offer")
	}
	if err := validateMovePath(destination); err != nil {
		return fileTransferTicket{}, err
	}
	if _, err := os.Lstat(destination); !os.IsNotExist(err) {
		return fileTransferTicket{}, fmt.Errorf("choose a new destination folder")
	}
	job, err := s.register("upload", offer)
	if err != nil {
		return fileTransferTicket{}, err
	}
	s.mu.Lock()
	if s.ctx.Err() != nil || job.ctx.Err() != nil {
		delete(s.jobs, job.ticket.Token)
		job.cancel()
		s.mu.Unlock()
		return fileTransferTicket{}, context.Canceled
	}
	job.destination = destination
	s.mu.Unlock()
	return job.ticket, nil
}

func (s *fileTransferService) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Cache-Control", "no-store")
	parts := strings.Split(strings.TrimPrefix(r.URL.Path, "/"), "/")
	if len(parts) != 2 || len(parts[1]) != 64 {
		http.NotFound(w, r)
		return
	}
	s.mu.Lock()
	job := s.jobs[parts[1]]
	if job == nil || job.ticket.Direction != parts[0] || (job.status.State != "transferring" && job.activeDownloads == 0 && !s.now().Before(job.expires)) || job.ctx.Err() != nil {
		s.mu.Unlock()
		http.NotFound(w, r)
		return
	}
	if r.Method == "DELETE" {
		s.release(job)
		delete(s.jobs, job.ticket.Token)
		s.mu.Unlock()
		w.WriteHeader(http.StatusNoContent)
		return
	}
	if parts[0] == "download" {
		file := job.file
		offer := job.ticket.Offer
		job.activeDownloads++
		job.expires = s.now().Add(transferTicketLifetime)
		s.mu.Unlock()
		defer func() {
			s.mu.Lock()
			job.activeDownloads--
			job.expires = s.now().Add(transferTicketLifetime)
			s.mu.Unlock()
		}()
		if file == nil || (r.Method != "GET" && r.Method != "HEAD") {
			http.Error(w, "Method unavailable", http.StatusMethodNotAllowed)
			return
		}
		select {
		case s.downloads <- struct{}{}:
			defer func() { <-s.downloads }()
		default:
			w.Header().Set("Retry-After", "1")
			http.Error(w, "Transfer busy", http.StatusServiceUnavailable)
			return
		}
		controller := http.NewResponseController(w)
		finished := make(chan struct{})
		stop := context.AfterFunc(job.ctx, func() { controller.SetWriteDeadline(time.Now()); close(finished) })
		defer func() {
			if !stop() {
				<-finished
			}
		}()
		w.Header().Set("Content-Type", "application/zip")
		w.Header().Set("ETag", `"`+offer.SHA256+`"`)
		reader := &transferDownloadReader{ReadSeeker: io.NewSectionReader(file, 0, offer.ArchiveBytes), progress: func(n int64) {
			s.mu.Lock()
			job.status = fileTransferStatus{State: "transferring", Bytes: n, Total: offer.ArchiveBytes, Phase: "Sending files"}
			s.mu.Unlock()
		}}
		http.ServeContent(w, r, "files.zip", time.Time{}, reader)
		s.mu.Lock()
		if r.Method == "GET" && r.Header.Get("Range") == "" && reader.bytes == offer.ArchiveBytes {
			job.status.State = "sent"
			job.status.Phase = "Files sent"
		} else if job.activeDownloads == 1 {
			job.status.State = "ready"
		}
		s.mu.Unlock()
		return
	}
	if r.Method == "GET" {
		status := job.status
		s.mu.Unlock()
		w.Header().Set("Content-Type", "application/json")
		json.NewEncoder(w).Encode(status)
		return
	}
	if r.Method != "POST" {
		s.mu.Unlock()
		http.Error(w, "Method unavailable", http.StatusMethodNotAllowed)
		return
	}
	if job.status.State == "completed" {
		s.mu.Unlock()
		w.WriteHeader(http.StatusNoContent)
		return
	}
	if job.status.State == "transferring" || job.attempts >= 3 {
		s.mu.Unlock()
		http.Error(w, "Transfer busy or retry limit reached", http.StatusConflict)
		return
	}
	offer, destination := job.ticket.Offer, job.destination
	if r.ContentLength >= 0 && r.ContentLength != offer.ArchiveBytes {
		s.mu.Unlock()
		http.Error(w, "Transfer length mismatch", http.StatusBadRequest)
		return
	}
	job.attempts++
	job.status = fileTransferStatus{State: "transferring", Total: offer.ArchiveBytes, Phase: "Receiving files"}
	s.mu.Unlock()
	ctx, cancel := context.WithTimeout(r.Context(), 2*time.Hour)
	defer cancel()
	stop := context.AfterFunc(job.ctx, cancel)
	defer stop()
	err := s.acquire(ctx)
	if err == nil {
		r.Body = http.MaxBytesReader(w, r.Body, offer.ArchiveBytes+1)
		body := r.Body
		bodyFinished := make(chan struct{})
		stopBody := context.AfterFunc(ctx, func() {
			http.NewResponseController(w).SetReadDeadline(time.Now())
			body.Close()
			close(bodyFinished)
		})
		defer func() {
			if !stopBody() {
				<-bodyFinished
			}
		}()
		err = receiveFileTransfer(ctx, r.Body, offer, destination, s.limits, func(bytes, total int64, phase string) {
			s.mu.Lock()
			job.status.Bytes = bytes
			job.status.Total = total
			job.status.Phase = phase
			s.mu.Unlock()
		})
		<-s.ioSlot
	}
	s.mu.Lock()
	if err == nil {
		job.status.State = "completed"
		job.status.Phase = "Files received"
	} else {
		job.status.State = "failed"
		job.status.Phase = "Transfer did not finish"
		if errors.Is(err, context.Canceled) {
			job.status.Phase = "Transfer cancelled"
		}
		if errors.Is(err, errInsufficientDiskSpace) {
			job.status.Phase = "Not enough space at the destination"
		}
	}
	job.expires = s.now().Add(transferTicketLifetime)
	phase := job.status.Phase
	s.mu.Unlock()
	if err != nil {
		http.Error(w, phase, http.StatusBadRequest)
		return
	}
	w.WriteHeader(http.StatusNoContent)
}

// Serve uses the listener chosen by the launcher. Tokens are distributed only
// over its clipboard/control channel, never through a public discovery route.
func (s *fileTransferService) Serve(listener net.Listener) error {
	server := &http.Server{
		Handler:           s,
		ReadHeaderTimeout: 5 * time.Second,
		ReadTimeout:       2 * time.Hour,
		WriteTimeout:      2 * time.Hour,
		IdleTimeout:       30 * time.Second,
		MaxHeaderBytes:    8192,
		BaseContext:       func(net.Listener) context.Context { return s.ctx },
	}
	done := make(chan struct{})
	stop := context.AfterFunc(s.ctx, func() { server.Close(); close(done) })
	defer func() {
		if !stop() {
			<-done
		}
	}()
	err := server.Serve(listener)
	if errors.Is(err, http.ErrServerClosed) && s.ctx.Err() != nil {
		return nil
	}
	return err
}

// ServeContent retains seek/range support while reporting bytes read for delivery.
type transferDownloadReader struct {
	io.ReadSeeker
	bytes    int64
	progress func(int64)
}

func (r *transferDownloadReader) Read(data []byte) (int, error) {
	n, err := r.ReadSeeker.Read(data)
	r.bytes += int64(n)
	if n > 0 {
		r.progress(r.bytes)
	}
	return n, err
}
