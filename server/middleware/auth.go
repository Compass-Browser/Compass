package middleware

import (
	"context"
	"net/http"

	"github.com/rs/zerolog/log"

	"github.com/brave/go-sync/auth"
	syncContext "github.com/brave/go-sync/synccontext"
)

// Auth verifies the token provided is valid, and sets the client id in context
func Auth(next http.Handler) http.Handler {
	return AuthWith(auth.Authorize)(next)
}

// AuthWith is Auth with |authorize| checking the token (Compass:
// auth.AuthorizeSync, whose login_required is answered 401
// {"error":"login_required"} with X-Compass-Login: required).
func AuthWith(authorize func(*http.Request) (string, error)) func(http.Handler) http.Handler {
	return func(next http.Handler) http.Handler {
		return authWith(authorize, next)
	}
}

func authWith(authorize func(*http.Request) (string, error), next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		clientID, err := authorize(r)
		if clientID == "" {
			if auth.WriteGateError(w, err) {
				return
			}
			if err != nil {
				log.Error().Err(err).Msg("Authorization failed")
			}
			http.Error(w, http.StatusText(http.StatusUnauthorized), http.StatusUnauthorized)
			return
		}

		ctx := context.WithValue(r.Context(), syncContext.ContextKeyClientID, clientID)
		next.ServeHTTP(w, r.WithContext(ctx))
	})
}
