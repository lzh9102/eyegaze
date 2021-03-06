#include <iostream>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <cstdlib>
#include "sound.h"

using namespace std;

namespace
{
	void mplayer(const char *filename)
	{
		int tmp_pid = fork();
		// Use double forking to reparent the inner child process to init.
		// This frees us from having to block to wait() for the child process.
		if (tmp_pid == 0) {
			int pid = fork();
			if (pid == 0) {
				int devnull = open("/dev/null", O_WRONLY);

				// redirect stdout and stderr to /dev/null
				close(1);         // close stdout
				close(2);         // close stderr
				dup2(devnull, 1); // devnull -> stdout
				dup2(devnull, 2); // devnull -> stderr
				close(devnull);   // close devnull

				execlp("mplayer", "mplayer", filename, NULL);
				cerr << "error: failed to execute mplayer" << endl;
				exit(1);
			}

			// Exit the outer child process so that the parent of the inner child
			// process will be set to init.
			exit(0);
		}
		waitpid(tmp_pid, NULL, 0);
	}
}

Sound::Sound(const char *filename) : filename_(filename)
{
}

void Sound::play() const
{
	mplayer(filename_);
}
