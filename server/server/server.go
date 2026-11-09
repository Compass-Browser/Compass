package server

import (
	"context"
	"errors"
	"fmt"
	"net"
	"net/http"
	_ "net/http/pprof" //nolint:gosec // G108: pprof is served on a separate listener (:6061), gated by PPROF_ENABLED
	"os"
	"os/signal"
	"path/filepath"
	"strconv"
	"strings"
	"syscall"
	"time"

	appctx "github.com/brave-intl/bat-go/libs/context"
	"github.com/brave-intl/bat-go/libs/handlers"
	"github.com/brave-intl/bat-go/libs/logging"
	batware "github.com/brave-intl/bat-go/libs/middleware"
	sentry "github.com/getsentry/sentry-go"
	"github.com/go-chi/chi/v5"
	chiware "github.com/go-chi/chi/v5/middleware"
	"github.com/rs/zerolog"
	"github.com/rs/zerolog/hlog"
	"github.com/rs/zerolog/log"

	"github.com/brave/go-sync/auth"
	"github.com/brave/go-sync/cache"
	"github.com/brave/go-sync/clientip"
	"github.com/brave/go-sync/command"
	"github.com/brave/go-sync/controller"
	"github.com/brave/go-sync/datastore"
	"github.com/brave/go-sync/events"
	"github.com/brave/go-sync/middleware"
	"github.com/brave/go-sync/pairing"
	"github.com/brave/go-sync/push"
	"github.com/brave/go-sync/quota"
	syncContext "github.com/brave/go-sync/synccontext"
)

var (
	commit            string
	version           string
	buildTime         string
	healthCheckActive = true
)

// Config is the server's configuration, from the environment (LoadConfig).
type Config struct {
	Addr             string        // COMPASS_SYNC_ADDR, default ":8295"
	DBPath           string        // COMPASS_SYNC_DB, default /data/compass-sync.db
	MaxBytesPerChain int64         // COMPASS_SYNC_MAX_BYTES, default 200 MiB
	TrustedProxies   string        // COMPASS_SYNC_TRUSTED_PROXIES, CIDRs
	Metrics          bool          // COMPASS_SYNC_METRICS: serve /metrics
	ShutdownDelay    time.Duration // COMPASS_SYNC_SHUTDOWN_DELAY, default 0
	PurgeInterval    time.Duration // How often expired history, old tombstones and idle chains are deleted.
	TombstoneDays    int           // COMPASS_SYNC_TOMBSTONE_DAYS, default 30; 0 = keep tombstones
	IdleChainDays    int           // COMPASS_SYNC_IDLE_CHAIN_DAYS, default 180; 0 = keep idle chains
	Backup           BackupConfig
	Pairing          pairing.Config
	Events           events.Config
	Push             push.Config // Topics and APNs; Store and Authorize are set by setupRouter.
}

// Defaults for tombstone purge, idle chains and backups (sync.md, "Server").
const (
	DefaultTombstoneDays = 30
	DefaultIdleChainDays = 180
	DefaultBackupKeep    = 7
)

// DefaultBackupInterval is how often the server backs up by default.
const DefaultBackupInterval = 24 * time.Hour

// envDays reads a number of days (0 or more) from the environment.
func envDays(name string, def int) (int, error) {
	v := os.Getenv(name)
	if v == "" {
		return def, nil
	}
	n, err := strconv.Atoi(v)
	if err != nil || n < 0 || n > 100000 {
		return def, fmt.Errorf("invalid %s %q (days, 0 = off)", name, v)
	}
	return n, nil
}

// DefaultMaxBytesPerChain is 200 MiB (sync.md, "Server").
const DefaultMaxBytesPerChain = 200 << 20

// LoadConfig reads the configuration from the environment.
// COMPASS_SYNC_MAX_ENTITIES is read by command.LoadQuotaFromEnv.
func LoadConfig() (Config, error) {
	cfg := Config{
		Addr:             ":8295",
		DBPath:           "/data/compass-sync.db",
		MaxBytesPerChain: DefaultMaxBytesPerChain,
		TrustedProxies:   os.Getenv("COMPASS_SYNC_TRUSTED_PROXIES"),
		PurgeInterval:    time.Hour,
		TombstoneDays:    DefaultTombstoneDays,
		IdleChainDays:    DefaultIdleChainDays,
		Backup:           BackupConfig{Interval: DefaultBackupInterval, Keep: DefaultBackupKeep},
		Pairing:          pairing.DefaultConfig(),
		Events:           events.DefaultConfig(),
	}
	if v := os.Getenv("COMPASS_SYNC_ADDR"); v != "" {
		cfg.Addr = v
	}
	if v := os.Getenv("COMPASS_SYNC_DB"); v != "" {
		cfg.DBPath = v
	}
	if v := os.Getenv("COMPASS_SYNC_MAX_BYTES"); v != "" {
		n, err := strconv.ParseInt(v, 10, 64)
		if err != nil || n < 0 {
			return cfg, fmt.Errorf("invalid COMPASS_SYNC_MAX_BYTES %q", v)
		}
		cfg.MaxBytesPerChain = n
	}
	if v := os.Getenv("COMPASS_SYNC_SHUTDOWN_DELAY"); v != "" {
		d, err := time.ParseDuration(v)
		if err != nil {
			return cfg, fmt.Errorf("invalid COMPASS_SYNC_SHUTDOWN_DELAY %q: %w", v, err)
		}
		cfg.ShutdownDelay = d
	}
	if v := os.Getenv("COMPASS_SYNC_EVENTS_PER_IP"); v != "" {
		n, err := strconv.Atoi(v)
		if err != nil || n <= 0 {
			return cfg, fmt.Errorf("invalid COMPASS_SYNC_EVENTS_PER_IP %q", v)
		}
		cfg.Events.MaxPerIP = n
	}
	var err error
	if cfg.TombstoneDays, err = envDays("COMPASS_SYNC_TOMBSTONE_DAYS", DefaultTombstoneDays); err != nil {
		return cfg, err
	}
	if cfg.IdleChainDays, err = envDays("COMPASS_SYNC_IDLE_CHAIN_DAYS", DefaultIdleChainDays); err != nil {
		return cfg, err
	}
	cfg.Backup.Dir = filepath.Join(filepath.Dir(cfg.DBPath), "backups")
	if v := os.Getenv("COMPASS_SYNC_BACKUP_DIR"); v != "" {
		cfg.Backup.Dir = v
	}
	if v := os.Getenv("COMPASS_SYNC_BACKUP_INTERVAL"); v != "" {
		d, err := time.ParseDuration(v)
		if v == "0" {
			d, err = 0, nil
		}
		if err != nil || d < 0 || (d > 0 && d < time.Minute) {
			return cfg, fmt.Errorf("invalid COMPASS_SYNC_BACKUP_INTERVAL %q (e.g. 24h, 0 = off)", v)
		}
		cfg.Backup.Interval = d
	}
	if v := os.Getenv("COMPASS_SYNC_BACKUP_KEEP"); v != "" {
		n, err := strconv.Atoi(v)
		if err != nil || n < 1 {
			return cfg, fmt.Errorf("invalid COMPASS_SYNC_BACKUP_KEEP %q", v)
		}
		cfg.Backup.Keep = n
	}
	cfg.Metrics, _ = strconv.ParseBool(os.Getenv("COMPASS_SYNC_METRICS"))
	// APNs for sent tabs (docs/design/send-tab.md, "Delivery"); without a
	// key, registrations are kept and nothing is pushed.
	cfg.Push.APNs = push.APNsConfig{
		KeyPath: os.Getenv("COMPASS_SYNC_APNS_KEY"),
		KeyID:   os.Getenv("COMPASS_SYNC_APNS_KEY_ID"),
		TeamID:  os.Getenv("COMPASS_SYNC_APNS_TEAM_ID"),
		Host:    os.Getenv("COMPASS_SYNC_APNS_HOST"),
	}
	// FCM for Compass for Android: a Firebase service account's JSON key.
	cfg.Push.FCM = push.FCMConfig{CredentialsPath: os.Getenv("COMPASS_SYNC_FCM_CREDENTIALS")}
	if cfg.Push.APNs.Enabled() && (cfg.Push.APNs.KeyID == "" || cfg.Push.APNs.TeamID == "") {
		return cfg, errors.New("COMPASS_SYNC_APNS_KEY needs COMPASS_SYNC_APNS_KEY_ID and COMPASS_SYNC_APNS_TEAM_ID")
	}
	if v := os.Getenv("COMPASS_SYNC_APNS_TOPICS"); v != "" {
		for _, t := range strings.Split(v, ",") {
			if t = strings.TrimSpace(t); t != "" {
				cfg.Push.Topics = append(cfg.Push.Topics, t)
			}
		}
	}
	// The name devices bind into pairing (sync.md, "Pairing API"); empty:
	// the Host each pairing request came to.
	cfg.Pairing.PublicHost = os.Getenv("COMPASS_SYNC_PUBLIC_HOST")
	return cfg, nil
}

type baseCtxFunc = func(net.Listener) context.Context

func setupLogger(ctx context.Context) (context.Context, *zerolog.Logger) {
	ctx = context.WithValue(ctx, appctx.EnvironmentCTXKey, os.Getenv("ENV"))
	ctx = context.WithValue(ctx, appctx.LogLevelCTXKey, zerolog.WarnLevel)
	return logging.SetupLogger(ctx)
}

// setupRouter builds the HTTP routes on an open datastore. The events hub
// must be closed on shutdown (it ends the open change streams).
func setupRouter(
	ctx context.Context,
	logger *zerolog.Logger,
	cfg Config,
	db *datastore.SQLite,
) (context.Context, *chi.Mux, *events.Hub, error) {
	command.LoadHighDeviceLimitClientIDsFromEnv()
	command.LoadQuotaFromEnv()
	command.LoadPollIntervalFromEnv()

	trusted, err := clientip.ParseTrusted(cfg.TrustedProxies)
	if err != nil {
		return ctx, nil, nil, err
	}
	ips := clientip.New(trusted)

	r := chi.NewRouter()

	r.Use(chiware.RequestID)
	r.Use(chiware.Heartbeat("/"))

	if logger != nil {
		// Also handles panic recovery
		r.Use(hlog.NewHandler(*logger))
		r.Use(hlog.UserAgentHandler("user_agent"))
		r.Use(hlog.RequestIDHandler("req_id", "Request-Id"))
		r.Use(batware.RequestLogger(logger))
	}

	r.Use(batware.BearerToken)
	r.Use(middleware.CommonResponseHeaders)

	c := cache.NewCache(cache.NewMemoryClient())
	instrumented := datastore.NewDatastoreWithPrometheus(db, "sqlite")

	// Provide datastore & cache via context, on the base context and on
	// every request (so the handler also works without BaseContext).
	ctx = context.WithValue(ctx, syncContext.ContextKeyDatastore, datastore.Datastore(db))
	ctx = context.WithValue(ctx, syncContext.ContextKeyCache, c)
	r.Use(func(next http.Handler) http.Handler {
		return http.HandlerFunc(func(w http.ResponseWriter, req *http.Request) {
			rctx := context.WithValue(req.Context(), syncContext.ContextKeyDatastore, datastore.Datastore(db))
			rctx = context.WithValue(rctx, syncContext.ContextKeyCache, c)
			next.ServeHTTP(w, req.WithContext(rctx))
		})
	})

	pairCfg := cfg.Pairing
	pairCfg.ClientIP = ips.IP
	pairCfg.Authorize = auth.Authorize
	evCfg := cfg.Events
	evCfg.ClientIP = ips.IP
	evCfg.Authorize = auth.Authorize
	evCfg.IsDisabled = func(r *http.Request, chain string) (bool, error) {
		return db.IsSyncChainDisabled(r.Context(), chain)
	}
	hub := events.New(evCfg)
	quotaHandler := quota.New(quota.Config{
		Store:     db,
		Authorize: auth.Authorize,
		Limits: func() quota.Limits {
			q := command.CurrentQuotas()
			return quota.Limits{
				MaxEntities: q.MaxEntities,
				MaxBytes:    cfg.MaxBytesPerChain,
				MaxHistory:  q.MaxHistory,
				HistoryDays: q.HistoryDays,
			}
		},
	})
	pushCfg := cfg.Push
	pushCfg.Store = db
	pushCfg.Authorize = auth.Authorize
	pusher, err := push.New(pushCfg)
	if err != nil {
		return ctx, nil, nil, err
	}
	notify := func(chain, cacheGUID string, types, created []int) {
		hub.Notify(chain, cacheGUID, types)
		pusher.OnCommit(chain, cacheGUID, created)
	}
	r.Route("/v2", func(r chi.Router) {
		// Compass: change notifications (sync.md, "Change notifications"), a
		// long-lived stream, so outside the request timeout below.
		r.Method(http.MethodGet, "/compass/events", hub)
		r.Group(func(r chi.Router) {
			r.Use(chiware.Timeout(60 * time.Second))
			// Compass: pairing relay (sync.md, "Pairing API"), next to the
			// protocol endpoint and outside its auth middleware.
			r.Mount("/compass/pair", pairing.New(pairCfg).Routes())
			// Compass: how full the chain is (sync.md, "Server").
			r.Method(http.MethodGet, "/compass/quota", quotaHandler)
			// Compass: APNs tokens, for sent tabs (send-tab.md).
			r.Handle("/compass/push", pusher)
			r.Mount("/", controller.SyncRouter(c, instrumented, notify))
		})
	})
	if cfg.Metrics {
		r.With(chiware.Timeout(60*time.Second)).Get("/metrics", batware.Metrics())
	}

	log.Info().
		Str("version", version).
		Str("commit", commit).
		Str("buildTime", buildTime).
		Msg("server starting up")

	healthCheckHandler := func(w http.ResponseWriter, r *http.Request) {
		if healthCheckActive {
			handlers.HealthCheckHandler(version, buildTime, commit, nil, nil)(w, r)
		} else {
			w.WriteHeader(http.StatusGone)
		}
	}
	r.Get("/health-check", healthCheckHandler)
	r.Get("/healthz", func(w http.ResponseWriter, r *http.Request) {
		if !healthCheckActive {
			http.Error(w, "shutting down", http.StatusServiceUnavailable)
			return
		}
		if err := db.Ping(r.Context()); err != nil {
			http.Error(w, "database unavailable", http.StatusServiceUnavailable)
			return
		}
		_, _ = w.Write([]byte("ok\n"))
	})
	return ctx, r, hub, nil
}

func setupBaseCtx(ctx context.Context) baseCtxFunc {
	return func(_ net.Listener) context.Context {
		return ctx
	}
}

// purge deletes expired history (DynamoDB's TTL did this for go-sync),
// tombstones older than cfg.TombstoneDays and chains idle for
// cfg.IdleChainDays.
func purge(ctx context.Context, db *datastore.SQLite, cfg Config, now time.Time) {
	n, err := db.PurgeExpired(ctx, now)
	if err != nil {
		log.Error().Err(err).Msg("Purging expired entities failed")
	} else if n > 0 {
		log.Info().Int64("count", n).Msg("Purged expired entities")
	}
	if cfg.TombstoneDays > 0 {
		n, err = db.PurgeTombstones(ctx, now.AddDate(0, 0, -cfg.TombstoneDays))
		if err != nil {
			log.Error().Err(err).Msg("Purging tombstones failed")
		} else if n > 0 {
			log.Info().Int64("count", n).Msg("Purged old tombstones")
		}
	}
	if cfg.IdleChainDays > 0 {
		chains, err := db.ExpireIdleChains(ctx, now.AddDate(0, 0, -cfg.IdleChainDays))
		if err != nil {
			log.Error().Err(err).Msg("Expiring idle chains failed")
		}
		for _, c := range chains {
			log.Info().Str("chainID", c).Int("days", cfg.IdleChainDays).Msg("Cleared and disabled an idle chain")
		}
	}
}

// purgeExpired runs purge now and every cfg.PurgeInterval, until ctx ends.
func purgeExpired(ctx context.Context, db *datastore.SQLite, cfg Config) {
	t := time.NewTicker(cfg.PurgeInterval)
	defer t.Stop()
	for {
		purge(ctx, db, cfg, time.Now())
		select {
		case <-ctx.Done():
			return
		case <-t.C:
		}
	}
}

// StartServer runs the sync server until SIGTERM or SIGINT.
func StartServer() {
	serverCtx, logger := setupLogger(context.Background())

	// Setup Sentry.
	sentryDsn := os.Getenv("SENTRY_DSN")
	if sentryDsn != "" {
		err := sentry.Init(sentry.ClientOptions{
			Dsn:     sentryDsn,
			Release: fmt.Sprintf("go-sync@%s-%s", commit, buildTime),
		})
		if err != nil {
			logger.Panic().Err(err).Msg("Init sentry failed")
		}
	}

	cfg, err := LoadConfig()
	if err != nil {
		logger.Fatal().Err(err).Msg("Invalid configuration")
	}
	db, err := datastore.NewSQLite(cfg.DBPath, datastore.SQLiteOptions{MaxBytesPerChain: cfg.MaxBytesPerChain})
	if err != nil {
		logger.Fatal().Err(err).Str("path", cfg.DBPath).Msg("Must be able to open the database to start")
	}

	logger.Info().Str("prefix", "main").Str("db", cfg.DBPath).Str("addr", cfg.Addr).Msg("Starting server")

	serverCtx, r, hub, err := setupRouter(serverCtx, logger, cfg, db)
	if err != nil {
		logger.Fatal().Err(err).Msg("Invalid configuration")
	}

	purgeCtx, stopPurge := context.WithCancel(serverCtx)
	go purgeExpired(purgeCtx, db, cfg)
	go backupLoop(purgeCtx, db, cfg.Backup)

	srv := http.Server{
		Addr:              cfg.Addr,
		Handler:           r,
		BaseContext:       setupBaseCtx(serverCtx),
		ReadHeaderTimeout: 10 * time.Second,
	}
	// Shutdown waits for handlers, and change streams never end on their
	// own: end them when it starts.
	srv.RegisterOnShutdown(hub.Close)

	sig := make(chan os.Signal, 1)
	signal.Notify(sig, syscall.SIGTERM, syscall.SIGINT)
	stopped := make(chan struct{})
	go func() {
		<-sig
		log.Info().Msg("Signal received, disabling health check")
		healthCheckActive = false // disable health check

		time.Sleep(cfg.ShutdownDelay)
		// Pairing long polls last up to 25 s.
		shutdownCtx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
		defer cancel()
		if err := srv.Shutdown(shutdownCtx); err != nil {
			log.Err(err).Msg("HTTP server graceful shutdown failed")
		}
		close(stopped)
	}()

	// Add profiling flag to enable profiling routes.
	if on, _ := strconv.ParseBool(os.Getenv("PPROF_ENABLED")); on {
		// pprof attaches routes to default serve mux
		// host:6061/debug/pprof/
		go func() {
			//nolint:gosec // G114: pprof diagnostic server
			if err := http.ListenAndServe("localhost:6061", http.DefaultServeMux); err != nil {
				log.Err(err).Msg("pprof service returned error")
			}
		}()
	}

	err = srv.ListenAndServe()
	if errors.Is(err, http.ErrServerClosed) {
		<-stopped
		log.Info().Msg("HTTP server closed")
	} else if err != nil {
		sentry.CaptureException(err)
		log.Error().Err(err).Msg("HTTP server start failed!")
	}
	stopPurge()
	if err := db.Close(); err != nil {
		log.Err(err).Msg("Closing the database failed")
	}
	if err != nil && !errors.Is(err, http.ErrServerClosed) {
		os.Exit(1)
	}
}
