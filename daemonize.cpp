/**
 * Copyright [2016]
 *
 * \author [Artur Troian <troian dot ap at gmail dot com>]
 * \author [Oleg Kravchenko <troian dot ap at gmail dot com>]
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <sys/file.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/resource.h>

#include <unistd.h>

#include <string>
#include <iostream>
#include <fstream>

#include <boost/filesystem.hpp>
#include <daemon/daemonize.hpp>

namespace daemonize {

static Json::Value *g_config = nullptr;
static int *g_lock_fd        = nullptr;
static cleanup_cb cleanup    = nullptr;
static void *cleanup_ctx     = nullptr;

void exit_daemon(int err) {
	if (cleanup) {
		cleanup(cleanup_ctx);
	}

	if (g_lock_fd) {
		if (*g_lock_fd > 0) {
			if (flock(*g_lock_fd, LOCK_UN) != 0) {
				std::cerr << "Can't unlock the lock file" << std::endl;
			}
			close(*g_lock_fd);
			*g_lock_fd = 0;
		}

		delete g_lock_fd;
	}

	if (!g_config->operator[]("pid_file").empty()) {
		unlink(g_config->operator[]("pid_file").asString().c_str());
	}

	delete g_config;

	_exit(err);
}

static int already_running(const std::string &lock_file) {
	int fd;

	fd = open(lock_file.c_str(), O_RDONLY, S_IRUSR | S_IWUSR);
	if (fd <= 0) {
		fprintf(stderr, "Can't open executable to lock: \"%s\": %s\n", lock_file.c_str(), strerror(errno));
		exit_daemon(EXIT_FAILURE);
	}

	if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
		fprintf(stderr, "Can't lock the lock file \"%s\". Is another instance running?\n", lock_file.c_str());
		exit_daemon(EXIT_FAILURE);
	}

	return fd;
}

static void write_pid(const std::string &pid_file) {
	try {
		std::ofstream file(pid_file, std::ios_base::trunc);
		file << std::to_string(getpid());
	} catch (const std::exception &e) {
		exit_daemon(EXIT_FAILURE);
	}
}

static void verify_config(Json::Value *config) {
	if (!config->isMember("env_dir")) {
		std::cerr << "Daemon config must provide \"env_dir\" member";
		exit_daemon(EXIT_FAILURE);
	}

	if (!config->isMember("as_daemon")) {
		std::cerr << "Daemon config must provide \"as_daemon\" member";
		exit_daemon(EXIT_FAILURE);
	}
}

pid_t make_daemon(Json::Value *config, cleanup_cb cb, void *userdata) {
	verify_config(config);

	int lock_fd = 0;
	if (config->isMember("lock_file")) {
		lock_fd = already_running(config->operator[]("lock_file").asString());
	}

	int *lock_pfd = nullptr;
	if (lock_fd != 0) {
		lock_pfd = new int;
		*lock_pfd = lock_fd;
	}

	if (config->operator[]("as_daemon").asBool()) {
		pid_t p = daemonize::detached::make();
		if (p != 0) {
			return p; // -V::773
		}
	}

	g_lock_fd   = lock_pfd;
	g_config    = config;
	cleanup     = cb;
	cleanup_ctx = userdata;

	// Setup environment dir
	if (chdir(config->operator[]("env_dir").asString().c_str()) < 0) {
		std::cerr << "error set env dir: " << strerror(errno) << std::endl;
		exit_daemon(EXIT_FAILURE);
	}

	// check of log directory exists
	std::string log_path(config->operator[]("env_dir").asString());
	if (log_path.back() != '/') {
		log_path += "/";
	}

	if (config->operator[]("log")["dir"].asString().substr(0, 1) != "/") {
		log_path += config->operator[]("log")["dir"].asString();
	}

	if (!boost::filesystem::exists(log_path)) {
		boost::filesystem::create_directory(log_path);
	}

	std::string std_file;
	Json::Value io_config;

	if (config->operator[]("io_mode").asString() == std::string("io_daemon")) {
		io_config = config->operator[]("io_daemon");
	} else {
		io_config = config->operator[]("io_debug");
	}

	if (io_config["stdin"].asString() != std::string("stdin")) {
		// stdin needs redirection
		if (io_config["stdin"].asString().compare("/dev/null") == 0) {
			std_file = "/dev/null";
		} else {
			std_file = config->operator[]("log")["dir"].asString();
			std_file.append("/");
			std_file.append(io_config["stdin"].asString());
		}

		close(STDIN_FILENO);

		int stdin_fd = open(std_file.c_str(), O_RDONLY);

		if (stdin_fd != 0) {
			if (stdin_fd > 0)
				close(stdin_fd);
			fprintf(stderr, "Unable to redirect stdin: Opened to: %d. Error: %s", stdin_fd, strerror(errno));
			exit_daemon(EXIT_FAILURE);
		}
	}

	if (io_config["stdout"].asString() != std::string("stdout")) {
		// stdout needs redirection
		if (io_config["stdout"].asString().compare("/dev/null") == 0) {
			std_file = "/dev/null";
		} else {
			std_file = config->operator[]("log")["dir"].asString();
			std_file.append("/");
			std_file.append(io_config["stdout"].asString());
		}

		close(STDOUT_FILENO);

		int stdout_fd = open(std_file.c_str(), O_CREAT | O_WRONLY | O_TRUNC);
		if (stdout_fd != 1) {
			if (stdout_fd > 0)
				close(stdout_fd);
			fprintf(stderr, "Unable to redirect stdout to: %s. Error: %s\n", std_file.c_str(), strerror(errno));
			exit_daemon(EXIT_FAILURE);
		}

		if (std_file.compare("/dev/null") != 0) {
			if (chmod(std_file.c_str(), 0644) < 0) {
				fprintf(stderr, "Unable change file permission: [%s]. Reason: %s\n", std_file.c_str(), strerror(errno));
				exit_daemon(EXIT_FAILURE);
			}
		}
	}

	if (io_config["stderr"].asString() != std::string("stderr")) {
		// stderr needs redirection
		if (io_config["stderr"].asString().compare("/dev/null") == 0) {
			std_file = "/dev/null";
		} else {
			std_file = config->operator[]("log")["dir"].asString();
			std_file.append("/");
			std_file.append(io_config["stderr"].asString());
		}

		close(STDERR_FILENO);

		int stderr_fd = open(std_file.c_str(), O_CREAT | O_WRONLY | O_TRUNC);

		if (stderr_fd != 2) {
			if (stderr_fd > 0)
				close(stderr_fd);
			fprintf(stderr, "Unable to redirect stderr to: %s. Error: %s", std_file.c_str(), strerror(errno));
			exit_daemon(EXIT_FAILURE);
		}

		if (std_file.compare("/dev/null") != 0) {
			if (chmod(std_file.c_str(), 0644) < 0) {
				fprintf(stderr, "Unable change file permission: [%s]. Reason: %s", std_file.c_str(), strerror(errno));
				exit_daemon(EXIT_FAILURE);
			}
		}
	}

	rlimit core_limits = {};
	core_limits.rlim_cur = core_limits.rlim_max = (rlim_t)RLIM_INFINITY;

	if (setrlimit(RLIMIT_CORE, &core_limits) < 0) {
		fprintf(stderr, "Unable to set rlimits. Error: %s", strerror(errno));
		exit_daemon(EXIT_FAILURE);
	}

	if (!g_config->operator[]("pid_file").empty()) {
		write_pid(config->operator[]("pid_file").asString());
	}

	return 0;
}

} // namespace daemonize
