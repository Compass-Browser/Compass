package auth

import "context"

// Authenticate is authenticate without a request's context.
func Authenticate(tkn string) (string, error) {
	return authenticate(context.Background(), tkn)
}
