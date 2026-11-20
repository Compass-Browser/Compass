// Package main is the Compass sync server (a fork of brave/go-sync).
//
//	compass-sync-server              serve (configured by COMPASS_SYNC_* env)
//	compass-sync-server healthcheck  exit 0 if the local server answers /healthz
//	compass-sync-server backup FILE  write a consistent copy of the database
//	                                 (FILE "-": to stdout)
//	compass-sync-server users list | revoke EMAIL|SUB [--delete-data] | restore EMAIL|SUB
//	compass-sync-server chains list  company sign-in: users and signed-in chains
//	compass-sync-server devices list CHAIN | remove CHAIN CACHE_GUID
//	                                 a chain's devices; remove one from it
//	compass-sync-server rooms list | show ROOM | delete ROOM
//	                                 shared rooms (sizes and members, never contents)
package main

import (
	"fmt"
	"os"

	"github.com/brave/go-sync/server"
)

func main() {
	if len(os.Args) > 1 {
		switch os.Args[1] {
		case "serve":
		case "healthcheck":
			os.Exit(server.Healthcheck())
		case "backup":
			os.Exit(server.Backup(os.Args[2:]))
		case "users":
			os.Exit(server.Users(os.Args[2:], os.Stdout, os.Stderr))
		case "chains":
			os.Exit(server.Chains(os.Args[2:], os.Stdout, os.Stderr))
		case "devices":
			os.Exit(server.Devices(os.Args[2:], os.Stdout, os.Stderr))
		case "rooms":
			os.Exit(server.Rooms(os.Args[2:], os.Stdout, os.Stderr))
		default:
			fmt.Fprintln(os.Stderr, "usage: compass-sync-server [serve | healthcheck | backup FILE | users ... | chains list | devices ... | rooms ...]")
			os.Exit(2)
		}
	}
	server.StartServer()
}
