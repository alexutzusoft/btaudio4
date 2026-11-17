package main

import (
	"context"
	"errors"
	"flag"
	"log"
	"net/http"
	"os"
	"os/signal"
	"syscall"
	"time"
)

func envOr(key, fallback string) string {
	if v := os.Getenv(key); v != "" {
		return v
	}
	return fallback
}

func defaultAddr() string {
	if v := os.Getenv("BTA_ADDR"); v != "" {
		return v
	}
	if p := os.Getenv("PORT"); p != "" {
		if p[0] == ':' {
			return p
		}
		return ":" + p
	}
	return ":8080"
}

func main() {
	addr := flag.String("addr", defaultAddr(), "HTTP listen address")
	dataDir := flag.String("data", envOr("BTA_DATA_DIR", "./data"), "directory to store diagnostic logs")
	verbose := flag.Bool("v", os.Getenv("BTA_VERBOSE") == "1", "log each incoming chunk")
	flag.Parse()

	store, err := NewStore(*dataDir)
	if err != nil {
		log.Fatalf("init store: %v", err)
	}

	srv := &http.Server{
		Addr:              *addr,
		Handler:           NewServer(store, *verbose),
		ReadHeaderTimeout: 5 * time.Second,
		ReadTimeout:       15 * time.Second,
		WriteTimeout:      15 * time.Second,
		IdleTimeout:       60 * time.Second,
	}

	stop := make(chan os.Signal, 1)
	signal.Notify(stop, syscall.SIGINT, syscall.SIGTERM)

	go func() {
		<-stop
		ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
		defer cancel()
		_ = srv.Shutdown(ctx)
		store.Close()
	}()

	log.Printf("BTAudio4 diagnostic server listening on %s (data=%s)", *addr, *dataDir)
	if err := srv.ListenAndServe(); err != nil && !errors.Is(err, http.ErrServerClosed) {
		store.Close()
		log.Fatalf("listen: %v", err)
	}
}
