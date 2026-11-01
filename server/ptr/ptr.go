// Package ptr has the pointer helpers go-sync used from the AWS SDK
// (aws.String, aws.Int64, ...), so the server no longer depends on it.
package ptr

// String returns a pointer to v.
func String(v string) *string { return &v }

// Bool returns a pointer to v.
func Bool(v bool) *bool { return &v }

// Int returns a pointer to v.
func Int(v int) *int { return &v }

// Int32 returns a pointer to v.
func Int32(v int32) *int32 { return &v }

// Int64 returns a pointer to v.
func Int64(v int64) *int64 { return &v }
