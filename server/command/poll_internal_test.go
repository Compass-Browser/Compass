package command

import (
	"testing"

	"github.com/stretchr/testify/assert"
)

func TestLoadPollIntervalFromEnv(t *testing.T) {
	defer func(old int32) { setSyncPollInterval = old }(setSyncPollInterval)
	assert.Equal(t, int32(DefaultPollSeconds), setSyncPollInterval)

	for _, tc := range []struct {
		env  string
		want int32
	}{
		{"", 900},
		{"60", 60},
		{"29", 60},    // Too short: ignored.
		{"86401", 60}, // Too long: ignored.
		{"soon", 60},
		{"30", 30},
	} {
		t.Setenv("COMPASS_SYNC_POLL_SECONDS", tc.env)
		LoadPollIntervalFromEnv()
		assert.Equal(t, tc.want, setSyncPollInterval, "env %q", tc.env)
	}
}
