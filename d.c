#include <fcntl.h>
#include <sys/sendfile.h>
#define _GNU_SOURCE

#include "d.h"
#include <dlfcn.h>
#include <errno.h>
#include <libgen.h>
#include <linux/limits.h>
#include <pwd.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

void error(const char *message) {
#define RED "\033[0;31m"
#define RESET "\033[0m"
	fprintf(stderr, RED "%s" RESET "\n", message);
}

__attribute__((noreturn)) void fail(const char *message) {
	error(message);
	exit(EXIT_FAILURE);
}
void deploy(char *, char *, bool, bool);

int main(int argc, char *argv[]) {
	static bool is_debug = false;
	static bool is_dry_run = false;

	char *config_file = NULL;
	char *config_dir = NULL;
	char *config_file2 = NULL;
	char *config_file3 = NULL;
	char *config_filename = NULL;
	char *so_file = NULL;
	char *cmd = NULL;
	char *deployment_name = NULL;
	void *handle = NULL;

	if (getenv("DEBUG") != NULL) {
		is_debug = true;
	}

	char *help_menu = "d: A dotfile manager.\n"
	                  "Commands: <deploy[ --deployment=* --dry] | undeploy[ --deployment=* --dry] | print>\n";

	enum Command {
		CommandNone,
		CommandDeploy,
		CommandUndeploy,
		CommandPrint,
	} command = CommandNone;

	if (argc < 2) {
		fprintf(stderr, "%s", help_menu);
		fail("Failed to recognize subcommand");
	}

	if (strcmp(argv[1], "deploy") == 0) {
		command = CommandDeploy;
	} else if (strcmp(argv[1], "undeploy") == 0) {
		command = CommandUndeploy;
	} else if (strcmp(argv[1], "print") == 0) {
		command = CommandPrint;
	} else {
		fprintf(stderr, "%s", help_menu);
		fail("Failed to recognize subcommand");
	}

	if ((command == CommandDeploy || command == CommandUndeploy) && argc > 2) {
		for (int i = 2; i < argc; i += 1) {
			if (strcmp(argv[i], "--dry") == 0) {
				is_dry_run = true;
			}

			if (strncmp(argv[i], "--deployment=", 13) == 0) {
				deployment_name = argv[i] + 13;
			}
		}
	}

	config_file = strdup(CONFIG_FILE);
	if (config_file == NULL) {
		error("Failed to copy config path");
		goto error;
	}
	config_file2 = strdup(config_file);
	if (config_file2 == NULL) {
		error("Failed to copy config path");
		goto error;
	}
	config_file3 = strdup(config_file);
	if (config_file3 == NULL) {
		error("Failed to copy config path");
		goto error;
	}
	config_dir = dirname(config_file2);
	config_filename = basename(config_file3);

	struct stat st;
	if (stat(config_file, &st) == -1) {
		perror("Failed to stat file");
		goto error;
	}
	if (S_ISDIR(st.st_mode)) {
		error("Config file must not be a directory");
		goto error;
	}

	if (asprintf(&so_file, "%s/libdotfiles.so", config_dir) == -1) {
		error("Failed to create library path");
		goto error;
	}

	if (asprintf(
	        &cmd,
	        "gcc -g -fPIC -c %s -DCONFIG_HOME=\"\\\"$HOME\\\"\" -o %s/%s.o && gcc -shared -o %s/libdotfiles.so %s/%s.o",
	        config_file, config_dir, config_filename, config_dir, config_dir, config_filename) == -1) {
		error("Failed to create compilation command");
		goto error;
	}

	// TODO
	printf("Executing: %s\n", cmd);
	int result = system(cmd);
	if (result == -1) {
		error("Failed to compile config");
		goto error;
	};
	if (!WIFEXITED(result) || (WIFEXITED(result) && WEXITSTATUS(result) != 0)) {
		error("Command failed to exit\n");
		goto error;
	}

	handle = dlopen(so_file, RTLD_LAZY);
	if (handle == NULL) {
		fprintf(stderr, "%s\n", dlerror());
		goto error;
	}
	dlerror();

	Deployment **(*getDeployments)(void) = (Deployment * *(*)(void)) dlsym(handle, "getDeployments");
	char *dl_error = dlerror();
	if (dl_error != NULL) {
		fprintf(stderr, "%s\n", dl_error);
		goto error;
	}

	Deployment *(*getDefaultDeployment)(void) = (Deployment * (*)(void)) dlsym(handle, "getDefaultDeployment");
	dl_error = dlerror();
	if (dl_error != NULL) {
		fprintf(stderr, "%s\n", dl_error);
		goto error;
	}

	Deployment **groups = getDeployments();
	if (groups == NULL) {
		fprintf(stderr, "Failed to get deployments\n");
		goto error;
	}

	Deployment *current_group = NULL;
	if (deployment_name == NULL) {
		current_group = getDefaultDeployment();
	} else {
		for (int i = 0; groups[i] != NULL; i++) {
			if (strcmp(groups[i]->name, deployment_name) == 0) {
				current_group = groups[i];
				break;
			}
		}
		if (current_group == NULL) {
			fprintf(stderr, "Deployment '%s' not found\n", deployment_name);
			goto error;
		}
	}

	if (current_group->entries == NULL) {
		fprintf(stderr, "Failed to get configuration\n");
		goto error;
	}

	if (command == CommandPrint)
		printf("[\n");
	for (int i = 0;; i += 1) {
		Entry *entrydeployment = current_group->entries[i];
		if (entrydeployment == NULL) {
			break;
		}

		for (int j = 0;; j += 1) {
			Entry entry = entrydeployment[j];
			if (entry.source == NULL && entry.destination == NULL) {
				break;
			}

			if (command == CommandPrint) {
				printf("\t{ \"source\": \"%s\", \"destination\": \"%s\" }", entry.source, entry.destination);
				if (current_group->entries[i + 1] == NULL) {
					printf("\n");
				} else {
					printf(",\n");
				}
				continue;
			}

			char *home = getenv("HOME");
			if (home == NULL) {
				perror("getenv");
				goto error;
			}
			if (command == CommandDeploy) {
				char source_path[PATH_MAX];
				char destination_path[PATH_MAX];
				snprintf(source_path, PATH_MAX, "%s", entry.source);
				snprintf(destination_path, PATH_MAX, "%s", entry.destination);
				deploy(source_path, destination_path, is_debug, is_dry_run);
			} else if (command == CommandUndeploy) {
				char source_path[PATH_MAX];
				char destination_path[PATH_MAX];
				snprintf(source_path, PATH_MAX, "%s", entry.source);
				snprintf(destination_path, PATH_MAX, "%s", entry.destination);
				if (destination_path[strlen(destination_path) - 1] == '/') {
					destination_path[strlen(destination_path) - 1] = '\0';
				}
				if (is_dry_run) {
					printf("[DRY RUN] Would unlink: %s\n", destination_path);
				} else {
					if (unlink(destination_path) == -1) {
						if (errno != ENOENT) {
							fprintf(stderr, "Failed to unlink \"%s\"\n", destination_path);
							perror("unlink");
							goto error;
						}
					}
				}
			}
		}
	}
	if (command == CommandPrint)
		printf("]\n");

error:
	if (handle)
		dlclose(handle);
	if (so_file)
		free(so_file);
	if (config_file)
		free(config_file);
	if (config_file2)
		free(config_file2);
	if (config_file3)
		free(config_file3);
	if (cmd)
		free(cmd);
	exit(1);
}

void deploy(char *source_path, char *destination_path, bool debug, bool dry_run) {
	if (debug) {
		printf("source_path: %s\ndestination_path: %s\n", source_path, destination_path);
	}

	// Check trailing slash.
	{
		if (destination_path[strlen(destination_path) - 1] == '/') {
			if (source_path[strlen(source_path) - 1] != '/') {
				fprintf(stderr,
				        "Error: If destination path does have trailing slash, then source path must have it too.\n");
				exit(1);
			}

			destination_path[strlen(destination_path) - 1] = '\0';
			source_path[strlen(source_path) - 1] = '\0';
		} else {
			if (source_path[strlen(source_path) - 1] == '/') {
				fprintf(
				    stderr,
				    "Error: If destination path does not have trailing slash, then source path must not have it either.\n");
				exit(1);
			}
		}

		struct stat st1 = {0};
		if (stat(source_path, &st1) == -1) {
			fail("Failed to find source path");
		}

		struct stat st2 = {0};
		if (stat(destination_path, &st2) != -1) {
			if (S_ISDIR(st1.st_mode) && !S_ISDIR(st2.st_mode)) {
				fail("Failed to match directory types");
			}
			if (!S_ISDIR(st1.st_mode) && S_ISDIR(st2.st_mode)) {
				fail("Failed to match directory types");
			}
		}
	}

	// Create parent directory if it does not exist.
	{
		char *dir = malloc(strlen(destination_path) + 1);
		if (dir == NULL) {
			error("Failed to allocate path memory");
			goto error;
		}

		strcpy(dir, destination_path);
		dirname(dir);
		if (dir == NULL) {
			error("Failed to extract directory name");
			goto error;
		}
		struct stat st = {0};
		if (stat(dir, &st) == -1) {
			if (errno != ENOENT) {
				fail("Failed to check path status");
			}

			if (dry_run) {
				printf("[DRY RUN] Would create directory: %s\n", dir);
			} else {
				printf("Creating directory for: %s\n", dir);
				if (mkdir(dir, 0755) == -1) {
					perror("mkdir");
					fail("Failed to make directory");
				}
			}
		}
		free(dir);

		goto end;
	error:
		free(dir);
		exit(1);
	end:;
	}

	struct stat st = {0};
	bool exists = true;
	if (lstat(destination_path, &st) == -1) {
		if (errno != ENOENT) {
			fail("Failed to check path status");
		}

		exists = false;
	}

	// TODO
	// This is bad logic for when outside of home there is a file not symlink
	// Also, shouldn't assume "home" is destination. They could have macro for that
	if (exists && !S_ISLNK(st.st_mode)) {
		printf("SKIPPING: %s\n", destination_path);
		return;
	}

	if (S_ISLNK(st.st_mode)) {
		if (dry_run) {
			printf("[DRY RUN] Would unlink existing symlink: %s\n", destination_path);
		} else {
			if (unlink(destination_path) == -1) {
				error("Failed to remove old link");
			}
		}
	}

	char *home = getenv("HOME");
	if (home == NULL) {
		fail("Failed to get home directory");
	}

	if (strncmp(destination_path, home, strlen(home)) == 0) {
		if (dry_run) {
			printf("[DRY RUN] Would symlink %s to %s\n", source_path, destination_path);
		} else {
			if (symlink(source_path, destination_path) == -1) {
				fail("Failed to create link");
			}
			printf("Symlinked %s to %s\n", source_path, destination_path);
		}
	} else {
		if (dry_run) {
			printf("[DRY RUN] Would copy %s to %s\n", source_path, destination_path);
		} else {
			int source_fd = open(source_path, O_RDONLY);
			if (source_fd == -1) {
				fail("Failed to open source file");
			}

			int dest_fd = open(destination_path, O_WRONLY | O_CREAT, 0644);
			if (dest_fd == -1) {
				close(source_fd);
				fail("Failed to create destination file");
			}

			struct stat stat_buf;
			if (fstat(source_fd, &stat_buf) == -1) {
				close(source_fd);
				close(dest_fd);
				fail("Failed to get file size");
			}

			if (sendfile(dest_fd, source_fd, NULL, stat_buf.st_size) == -1) {
				close(source_fd);
				close(dest_fd);
				fail("Failed to copy file");
			}

			close(source_fd);
			close(dest_fd);
			printf("Copied %s to %s\n", source_path, destination_path);
		}
	}
}
