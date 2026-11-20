package oidclogin

import (
	"context"
	"errors"
	"time"

	"github.com/rs/zerolog/log"
	"golang.org/x/oauth2"
)

// Run revalidates users every RevalidateTick until |ctx| ends.
func (s *Service) Run(ctx context.Context) {
	if !s.Enabled() || s.cfg.RevalidateEvery <= 0 {
		return
	}
	tick := s.cfg.RevalidateTick
	if tick <= 0 || tick > s.cfg.RevalidateEvery {
		tick = s.cfg.RevalidateEvery
	}
	t := time.NewTicker(tick)
	defer t.Stop()
	for {
		select {
		case <-ctx.Done():
			return
		case <-t.C:
		}
		s.Revalidate(ctx)
	}
}

// RevalidateResult counts what one Revalidate did.
type RevalidateResult struct {
	Checked int // Refreshed and still allowed.
	Ended   int // Users whose chains were unbound.
	Failed  int // Couldn't tell (identity provider unreachable): kept.
}

// Revalidate refreshes the refresh token of every user last checked
// RevalidateEvery ago. invalid_grant (the user was removed, disabled or
// signed out at the identity provider) or a refreshed ID token the policy
// now refuses unbinds the user's chains; any other failure keeps them, to
// try again next time.
func (s *Service) Revalidate(ctx context.Context) RevalidateResult {
	var res RevalidateResult
	if !s.Enabled() || s.cfg.RevalidateEvery <= 0 || !s.Ready() {
		return res
	}
	now := s.cfg.Now()
	users, err := s.cfg.Store.OIDCUsersToRevalidate(ctx, s.cfg.Issuer, now.Add(-s.cfg.RevalidateEvery))
	if err != nil {
		log.Error().Err(err).Msg("Sign-in: listing users to revalidate failed")
		return res
	}
	cctx := s.clientCtx(ctx)
	for _, u := range users {
		if ctx.Err() != nil {
			break
		}
		tok, err := s.oauth.TokenSource(cctx, &oauth2.Token{RefreshToken: u.RefreshToken}).Token()
		if err != nil {
			var re *oauth2.RetrieveError
			if errors.As(err, &re) && re.ErrorCode == "invalid_grant" {
				s.end(ctx, u.ID, u.Email, "the identity provider refused the refresh token")
				res.Ended++
				continue
			}
			log.Warn().Err(err).Str("email", u.Email).Msg("Sign-in: revalidation failed; keeping access")
			res.Failed++
			continue
		}
		email, name := "", ""
		if raw, _ := tok.Extra("id_token").(string); raw != "" {
			idt, err := s.verifier.Verify(cctx, raw)
			if err != nil {
				log.Warn().Err(err).Str("email", u.Email).Msg("Sign-in: refreshed ID token rejected; keeping access")
				res.Failed++
				continue
			}
			if idt.Subject != u.Subject {
				s.end(ctx, u.ID, u.Email, "the refreshed ID token is for another user")
				res.Ended++
				continue
			}
			who, err := s.check(idt)
			if err != nil {
				var d errDenied
				if errors.As(err, &d) {
					s.end(ctx, u.ID, u.Email, "no longer allowed: "+err.Error())
					res.Ended++
					continue
				}
				res.Failed++
				continue
			}
			email, name = who.email, who.name
		}
		refresh := ""
		if tok.RefreshToken != u.RefreshToken {
			refresh = tok.RefreshToken
		}
		if err = s.cfg.Store.UpdateOIDCUser(ctx, u.ID, refresh, email, name, now); err != nil {
			log.Error().Err(err).Msg("Sign-in: saving a revalidation failed")
			res.Failed++
			continue
		}
		res.Checked++
	}
	return res
}

func (s *Service) end(ctx context.Context, id int64, email, why string) {
	chains, err := s.cfg.Store.EndOIDCUserSessions(ctx, id, false)
	if err != nil {
		log.Error().Err(err).Msg("Sign-in: ending a user's sessions failed")
		return
	}
	s.forget(chains...)
	log.Info().Str("email", email).Int("chains", len(chains)).Str("reason", why).
		Msg("Sign-in: user's chains signed out")
}
