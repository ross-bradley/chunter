#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <limits.h>
#include <string.h>
#include <sys/types.h>
#include <regex.h>
#include <sys/stat.h>
#include <fcntl.h>

#define MODE_TEXT 1
#define MODE_HTML 2

typedef struct Filename Filename;
struct Filename {
  Filename*  prev;
  Filename*  next;
  char    filename[];
};

typedef struct Pattern Pattern;
struct Pattern {
  regex_t    regex;
  Pattern*  prev;
  Pattern*  next;
};

unsigned int     g_outputmode;
struct Pattern*    g_regexes;
struct Filename*  g_filenames;
char*        g_buffer;
char*        g_trash;
char*        g_basedir;

unsigned int     g_bufferSize;
unsigned int     g_trashSize;

void usage(char* arg0) {
  printf("Usage:\n\t%s -o [text|html] [-r <case-insensitive-regex>] [-R <case-sensitive-regex>] filename [filename ...]\n", arg0);
  exit(-1);
}

void compilePattern(char* pattern, int casesensitive) {
  struct Pattern* p = (struct Pattern*)calloc(1, sizeof(struct Pattern));
  int result = regcomp(&p->regex, pattern, casesensitive | REG_EXTENDED | REG_NOSUB);
  if (result == 0) {
    g_regexes->next = p;
    p->prev = g_regexes;
    p->next = NULL;
    g_regexes = p;
  } else {
    fprintf(stderr, "[-] WARNING: Skipping invalid regex : %s\n", pattern);
    free(p);
  }
}

void addFilename(char* filename) {
  struct Filename* f = (struct Filename*)calloc(1, sizeof(struct Filename) + strlen(filename) + 1);
  g_filenames->next = f;
  f->prev = g_filenames;
  strcpy(f->filename, filename);
  g_filenames = f;
}

void parseOpts(int argc, char* argv[]) {
  unsigned int optmap = 0;
  int c;

  while ((c = getopt (argc, argv, "o:r:R:")) != -1) {
      switch (c) {
        case 'o':
          if (!strncmp(optarg, "text", 4)) {
            g_outputmode = MODE_TEXT;
            optmap |= 1;
          } else if (!strncmp(optarg, "html", 4)) {
            g_outputmode = MODE_HTML;
            optmap |= 1;
          } else {
            fprintf(stderr, "[-] ERROR: output must be text|html\n", PATH_MAX);
            exit(-1);
          }
          break;

        case 'r':
          compilePattern(optarg, REG_ICASE);
          optmap |= 2;
          break;

        case 'R':
          compilePattern(optarg, 0);
          optmap |= 2;
          break;
        default:
          usage(argv[0]);
      }
  }

  // parse the positional arguments (i.e. filenames)
  for (c = optind; c < argc; c++) {
    addFilename(argv[c]);
    optmap |= 4;
  }

  // check that all mandatory arguments have been provided 
  // TODO: specify which options are actually missing
  if (optmap != 7) {
    usage(argv[0]);
  }

  // walk to the top of the file list
  struct Filename* f = (struct Filename*)calloc(1, sizeof(struct Filename));
  g_filenames->next = f;
  f = g_filenames;
  while(f->prev) {
    f = f->prev;
  }
  g_filenames = f->next;

  // walk to the top of the regex list
  struct Pattern* p = calloc(1, sizeof(struct Pattern));
  g_regexes->next = p;
  p = g_regexes;
  while(p->prev) {
    p = p->prev;
  }
  g_regexes = p->next;
}

void output(char* filename, int lineNum) {
  int idx, jdx = 0;

  switch (g_outputmode) {
    case MODE_TEXT:
      printf("%s:%d:%s\n", filename, lineNum, g_buffer);
      break;

    case MODE_HTML:
      // build the file path, column number values etc.
      snprintf(g_trash, g_trashSize, "<tr><td>%s/%s</td><td><a href=\"chunt://%s/%s:%d\">%d</a></td><td>", g_basedir, filename, g_basedir, filename, lineNum, lineNum);

      // we need to replace '<' and '>' with html entities to avoid them breaking the html output
      if (strchr(g_buffer, 0x3c) || strchr(g_buffer, 0x3e)) {
        printf("%s", g_trash);
        memset(g_trash, 0, 65535);

        for (idx = 0; idx < strlen(g_buffer); idx++) {
          if ((g_buffer[idx] != '<') && (g_buffer[idx] != '>')) {
            g_trash[jdx++] = g_buffer[idx];
          } else {
            switch (g_buffer[idx]) {
              case '<':
                strcat(g_trash, "&lt;");
                jdx += 4;
                break;

              case '>':
                strcat(g_trash, "&gt;");
                jdx += 4;
                break;
            }
          }
          if (jdx >= (g_trashSize - 4)) {
            g_trash[jdx] = '\0';
            printf("%s", g_trash);
            jdx = 0;
          }
        }
        g_trash[jdx] = '\0';
        printf("%s</td></tr>\n", g_trash);
      } else {
        printf("%s%s</td></tr>\n", g_trash, g_buffer);
      }
  }
}

void doRegex(char* filename, int lineNum) {
  int retval = 0;
  struct Pattern* p = g_regexes;
  while (p->next) {
    if (regexec(&p->regex, g_buffer, 0, NULL, 0) == 0) {
      output(filename, lineNum);
    }
    p = p->next;
  }
}

void processFile(char* filename) {
  char c = '\0', nextc = '\0', lastc = '\0', lastQuote = '\0';
  int ch, idx = 0;
  int lineNum = 1;
  int bStarted = 0, bInQuotes = 0, bInComment = 0, bHash = 0;

  FILE* fd = fopen(filename, "r");
  if (fd == NULL) {
    fprintf(stderr, "[-] ERROR: Could not open %s\n", filename);
    return;
  }

  while ((ch = fgetc(fd)) != EOF) {
    c = (int)ch;
    switch(c) {
      // Windows EoL - just bin it
      case '\r':
        break;

      case '\n':
        if (bHash) {
          g_buffer[idx++] = '\0';
          bStarted = 0;
          bHash = 0;
          idx = 0;
          c = '\0';
          doRegex(filename, lineNum);
        }
        lineNum++;
        break;

      // white space
      case ' ':
      case '\t':
        if (bStarted) {
          if ((lastc != ' ') && (lastc != '\t')) {
            g_buffer[idx++] = c;
          }
        }
        break;

      // escape
      // if it's followed by a newline, ignore it, otherwise we store the escape and the next char in the buffer
      case '\\':
        bStarted = 1;
        nextc = (char)fgetc(fd);
        if ((nextc != '\r') && (nextc != '\n')) {
          g_buffer[idx++] = c;
          g_buffer[idx++] = nextc;
          c = nextc;
        } else {
          c = lastc;
        }
        break;

      // quotes
      case '\x22':
      case '\x27':
      case '\x60':
        // TODO: we should probably check the file extension before treating backticks as quotes, but in reality we're not likely to meet naked backticks in C etc.
        bStarted = 1;
        g_buffer[idx++] = c;
        if (!bInQuotes) {
          bInQuotes = 1;
          lastQuote = c;
        } else if ((bInQuotes) && (c == lastQuote)) {
          bInQuotes = 0;
          lastQuote = '\0';
        }
        break;

      // forward slash
      // check the next char to see if this is a comment
      // skip comments, otherwise it's probably a division etc so we keep it
      case '/':
        bStarted = 1;
        if (bInQuotes) {
          g_buffer[idx++] = c;
        } else {
          nextc = (char)fgetc(fd);
          if (nextc == '/') {
            // line comment - eat the rest of the line
            c = lastc;
            fgets(g_trash, 65535, fd);
            lineNum++;
          } else if (nextc == '*') {
            // block comment - find the end of the block
            // we hold two chars at once; when we find a '/' we check the prev char was a '*'
            c = lastc;
            int _ch = '\0';
            while (1) {
              ch = fgetc(fd);
              if (ch == EOF) {
                // we ran out of file
                break;
              } else if ((_ch == (int)'*') && (ch == (int)'/')) {
                // we found the end of the comment block
                break;
              } else if (ch == (int)'\n') {
                lineNum++;
              }
              _ch = ch;
            }
          } else {
            g_buffer[idx++] == c;
          }
        }
        break;

      // define, include etc
      // just eat these lines
      case '#':
        if (!bStarted) {
          bStarted = 1;
          bHash = 1;
          //fgets(g_trash, 65535, fd);
          //lineNum++;
        } else {
          g_buffer[idx++];
        }
        break;

      case '{':
      case '}':
      case ';':
        g_buffer[idx++] = c;
        if (!bInQuotes) {
          g_buffer[idx++] = '\0';
          bStarted = 0;
          idx = 0;
          c = '\0';
          doRegex(filename, lineNum);
        }
        break;

      // any other character
      default:
        bStarted = 1;
        g_buffer[idx++] = c;
    }
    // check for buffer overflow (yes, it's a dirty kludge)
    // TODO: anything other than this
    if (idx >= g_bufferSize) {
      g_bufferSize += 1024;
      g_buffer = realloc(g_buffer, g_bufferSize);
    } 

    lastc = c;
  }
  fclose(fd);
}

int main(int argc, char* argv[]) {
  g_regexes = (struct Pattern*)calloc(1, sizeof(struct Pattern));
  g_filenames = (struct Filename*)calloc(1, sizeof(struct Filename));
  parseOpts(argc, argv);

  g_buffer = (char*)calloc(1, 65535);
  g_trash = (char*)calloc(1, 65535);
  g_basedir = getcwd(NULL, 0);

  g_bufferSize = 65535;
  g_trashSize = 65535;

  while(g_filenames->next) {
    processFile(g_filenames->filename);
    g_filenames = g_filenames->next;
    free(g_filenames->prev);
  }
}
