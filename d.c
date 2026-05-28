#define _GNU_SOURCE
#include <fcntl.h>
#include <sys/sendfile.h>

#include "d.h"
#include <dlfcn.h>
#include <errno.h>
#include <libgen.h>
#include <linux/limits.h>
#include <pwd.h>
#include <spawn.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
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
	char *config_file3 = NULL;
	char *config_filename = NULL;
	char *cache_dir = NULL;
	char *so_file = NULL;
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
	config_file3 = strdup(config_file);
	if (config_file3 == NULL) {
		error("Failed to copy config path");
		goto error;
	}
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

	const char *home = getenv("HOME");
	if (home == NULL) {
		error("HOME is not set");
		goto error;
	}
	const char *xdg_cache = getenv("XDG_CACHE_HOME");
	if (xdg_cache != NULL && xdg_cache[0] != '\0') {
		cache_dir = strdup(xdg_cache);
		if (cache_dir == NULL) {
			error("Failed to copy XDG_CACHE_HOME");
			goto error;
		}
	} else {
		if (asprintf(&cache_dir, "%s/.cache", home) == -1) {
			error("Failed to create cache dir path");
			goto error;
		}
	}
	if (mkdir(cache_dir, 0700) == -1 && errno != EEXIST) {
		perror("Failed to create cache directory");
		goto error;
	}

	if (asprintf(&so_file, "%s/libdotfiles.so", cache_dir) == -1) {
		error("Failed to create library path");
		goto error;
	}

	char *obj_file = NULL;
	if (asprintf(&obj_file, "%s/%s.o", cache_dir, config_filename) == -1) {
		error("Failed to create object file path");
		goto error;
	}

	{
		char *config_home = NULL;
		if (asprintf(&config_home, "-DCONFIG_HOME=\"%s\"", home) == -1) {
			error("Failed to create CONFIG_HOME define");
			free(obj_file);
			goto error;
		}

		char *const compile_argv[] = {"gcc", "-g", "-fPIC", "-c", config_file, config_home, "-o", obj_file, NULL};
		pid_t pid;
		if (posix_spawnp(&pid, "gcc", NULL, NULL, compile_argv, environ) != 0) {
			perror("gcc");
			free(obj_file);
			free(config_home);
			goto error;
		}
		int status;
		if (waitpid(pid, &status, 0) == -1 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
			error("Compilation failed");
			free(obj_file);
			free(config_home);
			goto error;
		}
		free(config_home);
	}
	{
		char *const link_argv[] = {"gcc", "-shared", "-o", so_file, obj_file, NULL};
		pid_t pid;
		if (posix_spawnp(&pid, "gcc", NULL, NULL, link_argv, environ) != 0) {
			perror("gcc");
			free(obj_file);
			goto error;
		}
		int status;
		if (waitpid(pid, &status, 0) == -1 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
			error("Linking failed");
			free(obj_file);
			goto error;
		}
	}
	free(obj_file);

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

	Deployment **deployments = getDeployments();
	if (deployments == NULL) {
		fprintf(stderr, "Failed to get deployments\n");
		goto error;
	}

	Deployment *current_deployment = NULL;
	if (deployment_name == NULL) {
		current_deployment = getDefaultDeployment();
	} else {
		for (int i = 0; deployments[i] != NULL; i++) {
			if (strcmp(deployments[i]->name, deployment_name) == 0) {
				current_deployment = deployments[i];
				break;
			}
		}
		if (current_deployment == NULL) {
			fprintf(stderr, "Deployment '%s' not found\n", deployment_name);
			goto error;
		}
	}

	if (current_deployment->items == NULL) {
		fprintf(stderr, "Failed to get configuration\n");
		goto error;
	}

	if (command == CommandPrint)
		printf("[\n");
	for (int i = 0; current_deployment->items[i] != NULL; i += 1) {
		Item *outer = current_deployment->items[i];

		Item **items;
		int count;

		if (outer->type == TYPE_GROUP) {
			items = outer->entries;
			for (count = 0; items[count] != NULL; count++)
				;
		} else {
			items = &outer;
			count = 1;
		}

		for (int j = 0; j < count; j += 1) {
			Item *item = items[j];
			bool last = (j == count - 1) && current_deployment->items[i + 1] == NULL;

			if (command == CommandPrint) {
				printf("\t{ \"source\": \"%s\", \"destination\": \"%s\" }", item->source, item->destination);
				printf(last ? "\n" : ",\n");
				continue;
			}

			if (command == CommandDeploy) {
				char source_path[PATH_MAX];
				char destination_path[PATH_MAX];
				snprintf(source_path, PATH_MAX, "%s", item->source);
				snprintf(destination_path, PATH_MAX, "%s", item->destination);
				deploy(source_path, destination_path, is_debug, is_dry_run);
			} else if (command == CommandUndeploy) {
				char source_path[PATH_MAX];
				char destination_path[PATH_MAX];
				snprintf(source_path, PATH_MAX, "%s", item->source);
				snprintf(destination_path, PATH_MAX, "%s", item->destination);
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
	if (cache_dir)
		free(cache_dir);
	if (so_file)
		free(so_file);
	if (config_file)
		free(config_file);
	if (config_file3)
		free(config_file3);

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
				for (char *p = dir + 1;; p++) {
					if (*p == '/' || *p == '\0') {
						char saved = *p;
						*p = '\0';
						if (debug) {
							printf("mkdir: %s\n", dir);
						}
						if (mkdir(dir, 0755) == -1 && errno != EEXIST) {
							perror("mkdir");
							fail("Failed to make directory");
						}
						if (saved == '\0')
							break;
						*p = saved;
					}
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
