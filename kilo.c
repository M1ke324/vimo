/*** includes ***/
#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE
#define KILO_QUIT_TIMES 3

#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <ctype.h>
#include <stdio.h>
#include <stdarg.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <string.h>

/*** defines ***/

#define KILO_VERSION "0.0.1"
#define CTRL_KEY(k) ((k) & 0x1f)
#define KILO_TAB_STOP 8

enum editorKey{
	/*ASCII code */
	ENTER = 13,
	ESC = 27,
	BACKSPACE=127,
	/*softcode*/
	ARROW_LEFT =1000,
	CTRL_ARROW_LEFT, //CTRL_ARROW_LEFT-1 == ARROW_LEFT
	ARROW_RIGHT,
	CTRL_ARROW_RIGHT, //CTRL_ARROW_RIGHT-1 == ARROW_RIGHT
	ARROW_UP,
	ARROW_DOWN,
	DEL_KEY,
	HOME_KEY,
	END_KEY,
	PAGE_UP,
	PAGE_DOWN
};

enum editorHighlight{
	HL_NORMAL=0,
	HL_COMMENT,
	HL_MLCOMMENT,
	HL_KEYWORD1,
	HL_KEYWORD2,
	HL_STRING,
	HL_NUMBER,
	HL_MATCH
};

#define HL_HIGHLIGHT_NUMBERS (1<<0)
#define HL_HIGHLIGHT_STRINGS (1<<1)
#define HLDB_ENTRIES (sizeof(HLDB) / sizeof(HLDB[0]))

enum mode{
	NORMAL_MODE,
	INSERT_MODE,
	VISUAL_MODE,
	COMAND_MODE
};

const char* nameMode[]={"NORMAL MODE", "INSERT_MODE", "VISUAL MODE", "COMAND_MODE"};

/*** data ***/

struct editorSyntax{
	char *filetype;
	char **filematch;
	char **keywords;
	char *singleline_comment_start;
	char *multiline_comment_start;
	char *multiline_comment_end;
	int flags;
};

typedef struct erow {
	int idx; 			/* Row index in the file, zero-based*/
	int size; 			/*size of the row, excluding th null term*/
	int rsize;			/*size of the render row*/
	char *chars; 		/* row content*/
	char *render; 		/* Row content "rendered" for screen (for TABs). */
	unsigned char *hl; 	/* Syntax highlight type for each character in render.*/
	int hl_open_comment;/* Row had open comment at end in last syntax highlight check. */
} erow;

struct editorConfig {
	int cx,cy; 			/* Cursor x and y position in characters */
	int rx;				/* Render x position*/
	int rowoff;			/* Offset of row displayed. */
	int coloff;			/* Offset of column displayed. */
	int screenrows;		/* Number of rows that we can show */
	int screencols;		/* Number of cols that we can show */
	int numrows;		/* Number of rows */
	erow *row;
	int dirty;			/* File modified but not saved. */
	char *filename;		/* Currently open filename */
	char statusmsg[80];
	time_t statusmsg_time;
	struct editorSyntax *syntax;
	struct termios orig_termios;
	int mode;
	int quitTimes;
	int rowPadding;
  int startVisualModeCx, startVisualModeCy;
  char *copyBuffer;
};

struct editorConfig E;

/*** filetypes ***/
/*
 * In order to add a new syntax, define two arrays with a list of file name
 * matches and keywords. The file name matches are used in order to match
 * a given syntax with a given file name: if a match pattern starts with a
 * dot, it is matched as the last past of the filename, for example ".c".
 * Otherwise the pattern is just searched inside the filenme, like "Makefile").
 *
 * The list of keywords to highlight is just a list of words, however if they
 * a trailing '|' character is added at the end, they are highlighted in
 * a different color, so that you can have two different sets of keywords.
 *
 * Finally add a stanza in the HLDB global variable with two two arrays
 * of strings, and a set of flags in order to enable highlighting of
 * comments and numbers.
 *
 * The characters for single and multi line comments must be exactly two
 * and must be provided as well (see the C language example).
 *
 * There is no support to highlight patterns currently. */

// C/C++ highlight
char *C_HL_extensions[] = {".c", ".h", ".cpp", NULL};

char *C_HL_keywords[]={
	"switch", "if", "while", "for", "break", "cotinue", "return", "else",
	"struct", "union", "typedef", "static", "enum", "class", "case",

	"int|", "long|", "double|", "float|", "char|", "unsigned|", "signed|", "void|", NULL
};

struct editorSyntax HLDB[] = {
	{
		"c",
		C_HL_extensions,
		C_HL_keywords,
		"//","/*","*/",
		HL_HIGHLIGHT_NUMBERS | HL_HIGHLIGHT_STRINGS
	},
};

/*** prototypes ***/

void editorSetStatusMessage(const char *fmt, ...);
void editorRefreshScreen();
char *editorPrompt(char *prompt, void (*callback)(char *, int));
int numDigits(int n);

/*** terminal ***/

void die(const char *s){
	//Clear terminal
	write(STDOUT_FILENO,"\x1b[2J", 4);
	write(STDOUT_FILENO,"\x1b[H", 3);

	perror(s);
	exit(1);
}

void disableRawMode(){
	if(tcsetattr(STDIN_FILENO,TCSAFLUSH, &E.orig_termios) == -1)
		die("tcsetattr");
}

void enableRawMode(){
	if(tcgetattr(STDIN_FILENO, &E.orig_termios) == -1)
		die("tcgetattr");
	atexit(disableRawMode);
	struct termios raw = E.orig_termios;
	//Disable post-processing of output
	raw.c_oflag &= ~(OPOST);
	//Disable ctrl+S, ctrl+Q, ctrl+M, ctrl+J, altri flag per la raw mode
	raw.c_iflag &= ~(BRKINT | ISTRIP | INPCK |IXON | ICRNL);
	//Disable echo, ctrl+C, ctrl+Z, ctrl+V
	raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
	//Set caracther size to 8bit
	raw.c_cflag |= (CS8);
	//Read return as soon as ther as input
	raw.c_cc[VMIN] = 0;
	//return after 1/10s if no input
	raw.c_cc[VTIME] = 1;

	if(tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1)
		die("tcsetattr");
}

int editorReadKey(){
	int nread;
	char c;
	while((nread = read(STDIN_FILENO, &c, 1)) != 1){
		if(nread == -1 && errno != EAGAIN)
			die("read");
	}
	
	if(c == '\x1b'){
		char seq[5];

		/* If this is just an ESC, we'll timeout here. */
		if (read(STDIN_FILENO, seq, 1) == 0) return ESC;
		if (read(STDIN_FILENO, seq+1, 1) == 0) return ESC;
		// In the case of multimple argument by the terminal
		if (read(STDIN_FILENO, seq+2, 1)!=0) 
			if (read(STDIN_FILENO, seq+3, 1)!=0)
					read(STDIN_FILENO, seq+4, 1);
	
		if(seq[0] == '['){ //Escape sequence

			//case of 2 arguments like: \x1b[1;5A 
			if (seq[2] == ';'){
				if(seq[1]=='1' && seq[3]=='5' && seq[4]=='C')
					return CTRL_ARROW_RIGHT;
				if(seq[1]=='1' && seq[3]=='5' && seq[4]=='D')
					return CTRL_ARROW_LEFT;
			}

			if(seq[1] >='0' && seq[1] <= '9'){
				if(read(STDIN_FILENO, &seq[2], 1) != 1)
					return '\x1b';
				if(seq[2] == '~'){
					switch(seq[1]){
						case '1':
							return HOME_KEY;
						case '3':
							return DEL_KEY;
						case '4':
							return END_KEY;
						case '5': 
							return PAGE_UP;
						case '6':
							return PAGE_DOWN;
						case '7':
							return HOME_KEY;
						case '8':
							return END_KEY;
					}
				}
			}else{
				switch(seq[1]){
					case 'A': 
						return ARROW_UP;
					case 'B': 
						return ARROW_DOWN;
					case 'C': 
						return ARROW_RIGHT;
					case 'D': 
						return ARROW_LEFT;
					case 'H': 
						return HOME_KEY;
					case 'F': 
						return END_KEY;
							
				}
			}
		} else{
			if(seq[0] == '0'){
				switch(seq[1]){
					case 'H':
						return HOME_KEY;
					case 'F':
						return END_KEY;					
				}
			}
		}
		return '\x1b';
	}else
		return c;
}

int getCursorPosition(int *rows, int *cols){
	char buf[32];
	unsigned int i=0;
	if(write(STDOUT_FILENO,"\x1b[6n", 4) != 4) 
		return -1;
	
	while (i < sizeof(buf)-1){
		if(read(STDIN_FILENO, &buf[i], 1) != 1)
			break;
		if(buf[i]=='R')
			break;
		i++;	
	}
	buf[i]='\0';
	
	if(buf[0] != '\x1b' || buf[1] != '[') return -1;
	if(sscanf(&buf[2], "%d;%d", rows, cols) != 2) return -1;

	return 0;
}

int getWindowSize(int *rows, int *cols){
	struct winsize ws;

	if(ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws)==-1 || ws.ws_col==0){
		if(write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12)
			return -1;
		return getCursorPosition(rows, cols);
	}else{
		*cols=ws.ws_col;
		*rows=ws.ws_row;
		return 0;
	}
}

/*** syntax highlighting ***/

int is_separator(int c){
	return isspace(c) || c=='\0' || strchr(",.()+-/*=~%<>[];",c) != NULL;
}

void editorUpdateSyntax(erow *row){
	printf("Before realloc: row->rsize = %d, row->hl = %p\n", row->rsize, row->hl);
	row->hl = realloc(row->hl, row->rsize);
	printf("After realloc: row->hl = %p\n", row->hl);
	memset(row->hl, HL_NORMAL, row->rsize);
	printf("After memset: completed successfully\n");

	if(E.syntax == NULL)
		return;

	char **keywords =E.syntax->keywords;

	char *scs =E.syntax->singleline_comment_start;
	char *mcs =E.syntax->multiline_comment_start;
	char *mce =E.syntax->multiline_comment_end;

	int scs_len=scs ? strlen(scs) : 0;
	int mcs_len=mcs ? strlen(mcs) : 0;
	int mce_len=mce ? strlen(mce) : 0;

	int prev_sep=1;
	int in_string=0;
	int in_comment=(row->idx>0&&E.row[row->idx-1].hl_open_comment);

	int i=0;
	while(i<row->rsize){
		char c=row->render[i];
		unsigned char prev_hl = (i>0) ? row->hl[i - 1] : HL_NORMAL;

		if(scs_len && !in_string && !in_comment){
			if(!strncmp(&row->render[i], scs, scs_len)){
				memset(&row->hl[i], HL_COMMENT, row->rsize-i);
				break;
			}
		}

		if(mcs_len && mce_len && !in_string){
			if(in_comment){
				row->hl[i]=HL_MLCOMMENT;
				if(!strncmp(&row->render[i], mce, mce_len)){
					memset(&row->hl[i], HL_MLCOMMENT, mce_len);
					i+=mce_len;
					in_comment=0;
					prev_sep=1;
					continue;
				}else{
					i++;
					continue;
				}
			}else if(!strncmp(&row->render[i],mcs,mcs_len)){
				memset(&row->hl[i], HL_MLCOMMENT, mcs_len);
				i+=mcs_len;
				in_comment=1;
				continue;
			}
		}

		if(E.syntax->flags & HL_HIGHLIGHT_STRINGS){
			if(in_string){
				row->hl[i]=HL_STRING;
				if(c=='\\' && i+1<row->rsize){
					row->hl[i+1]=HL_STRING;
					i+=2;
					continue;
				}
				if(c==in_string)
					in_string=0;
				i++;
				prev_sep=1;
				continue;
			}else{
				if(c=='"' || c=='\''){
					in_string=c;
					row->hl[i]=HL_STRING;
					i++;
					continue;
				}
			}
		}
		
		if(E.syntax->flags & HL_HIGHLIGHT_NUMBERS){
			if((isdigit(c) && (prev_sep || prev_hl == HL_NUMBER)) || (c=='.' && prev_hl==HL_NUMBER)){
				row->hl[i]=HL_NUMBER;
				i++;
				prev_sep=0;
				continue;
			}
		}

		if(prev_sep){
			int j;
			for(j=0; keywords[j];j++){
				int klen = strlen(keywords[j]);
				int kw2 = keywords[j][klen-1]=='|';
				if(kw2) 
					klen--;

				if(!strncmp(&row->render[i], keywords[j], klen) && is_separator(row->render[i+klen])){
					memset(&row->hl[i], kw2 ? HL_KEYWORD2 : HL_KEYWORD1,klen);
					i+=klen;
					break;
				}
			}
			if(keywords[j] != NULL){
				prev_sep=0;
				continue;
			}
		}

		prev_sep=is_separator(c);
		i++;
	}

	int changed=(row->hl_open_comment!=in_comment);
	row->hl_open_comment=in_comment;
	if(changed && row->idx+1<E.numrows)
		editorUpdateSyntax(&E.row[row->idx+1]);
}

int editorSyntaxToColor(int hl){
	switch(hl){
		case HL_COMMENT:
		case HL_MLCOMMENT:
			return 36;
		case HL_KEYWORD1:
			return 33;
		case HL_KEYWORD2:
			return 32;
		case HL_STRING:
			return 35;
		case HL_NUMBER:
			return 31;
		case HL_MATCH:
			return 34;
		default:
			return 37;
	}
}

void editorSelectSyntaxHighlight(){
	E.syntax=NULL;
	if(E.filename == NULL)
		return;

	char *ext =strrchr(E.filename, '.');

	for(unsigned int j=0;j<HLDB_ENTRIES;j++){
		struct editorSyntax *s = &HLDB[j];
		unsigned int i = 0;
		while(s->filematch[i]){
			int is_ext = (s->filematch[i][0]=='.');
			if((is_ext && ext && !strcmp(ext, s->filematch[i])) ||
				(!is_ext && strstr(E.filename, s->filematch[i]))){
				E.syntax=s;
				
				int filerow;
				for(filerow=0;filerow<E.numrows;filerow++){
					editorUpdateSyntax(&E.row[filerow]);
				}

				return;
			}
			i++;
		}
	}
}

/*** row operations ***/

int editorRowCxToRx(erow *row, int cx){
	int rx=0;
	int j;

	for(j=0;j<cx;j++){
		if(row->chars[j]=='\t')
			rx+= (KILO_TAB_STOP-1)-(rx%KILO_TAB_STOP);
		rx++;
	}
	return rx+E.rowPadding;
}

int editorRowRxToCx(erow *row, int rx){
	int cur_rx=0;
	int cx=0;

	for(cx=0;cx<row->size;cx++){
		if(row->chars[cx]=='\t')
			cur_rx+=(KILO_TAB_STOP-1)-((cur_rx%KILO_TAB_STOP));
		cur_rx++;

		if(cur_rx>rx)
			return cx;
	}
	return cx;
}

void editorUpdateRow(erow *row){Se la prendete a keplero un pane arabo
	int tabs =0;
	int j;

	for(j=0;j<row->size;j++)
		if(row->chars[j]=='\t')
			tabs++;

	free(row->render);
	row->render=malloc(row->size + tabs*(KILO_TAB_STOP - 1) + 1);

	int idx=0;

	for(j=0;j<row->size;j++){
		if(row->chars[j]=='\t'){
			row->render[idx++]=' ';
			while(idx % KILO_TAB_STOP != 0)
				row->render[idx++]=' ';
		}else{
			row->render[idx++]=row->chars[j];
		}
	}

	row->render[idx]='\0';
	row->rsize=idx;

	editorUpdateSyntax(row);
}

void editorInsertRow(int at, char *s, size_t len){
	if(at<0 || at> E.numrows)
		return;

	E.row = realloc(E.row,sizeof(erow) * (E.numrows + 1));
	memmove(&E.row[at+1], &E.row[at], sizeof(erow)*(E.numrows-at));
	for(int j=at+1;j<=E.numrows;j++) E.row[j].idx++;

	E.row[at].idx=at;

	E.row[at].size = len;
	E.row[at].chars = malloc(len+1);
	memcpy(E.row[at].chars, s, len);
	E.row[at].chars[len] = '\0';

	E.row[at].rsize=0;
	E.row[at].render=NULL;
	E.row[at].hl=NULL;
	E.row[at].hl_open_comment=0;
	editorUpdateRow(&E.row[at]);

	E.numrows++;
	E.rowPadding=numDigits(E.numrows);
	E.dirty++;
}

void editorFreeRow(erow *row){
	free(row->render);
	free(row->chars);
	free(row->hl);
}

void editorDelRow(int at){
	if(at<0 || at>=E.numrows)
		return;
	editorFreeRow(&E.row[at]);
	memmove(&E.row[at], &E.row[at+1],sizeof(erow)*(E.numrows-at-1));
	for(int j=at;j<E.numrows-1;j++) 
		E.row[j].idx--;
	E.numrows--;
	E.rowPadding=numDigits(E.numrows);
	E.dirty++;
}

void editorRowInsertChar(erow *row, int at, int c){
	if(at<0||at>row->size)
		at=row->size;
	row->chars=realloc(row->chars,row->size +2);
	memmove(&row->chars[at+1], &row->chars[at], row->size-at+1);
	row->size++;
	row->chars[at]=c;
	editorUpdateRow(row);
	E.dirty++;
}

void editorRowAppendString(erow *row, char *s, size_t len){
	row->chars = realloc(row->chars, row->size+len+1);
	memcpy(&row->chars[row->size], s, len);
	row->size+=len;
	row->chars[row->size]='\0';
	editorUpdateRow(row);
	E.dirty++;
}

void editorRowDelChar(erow *row, int at){
	if(at<0 || at >= row->size)
		return;
	memmove(&row->chars[at], &row->chars[at+1], row->size-at);
	row->size--;
	editorUpdateRow(row);
	E.dirty++;
}

int editorRowGetChars(erow *row, int at, int nChars, char *buffer){
  if(at<0 || at>=E.numrows)
	  return;
  int nCopy=(row->size-at<nChars)? row->size-at : nChars;
  memcpy(&row->chars[at], buffer, nCopy);
  return nChars-nCopy;
}

/*** editor operations ***/
void editorNewCopyBuffer(){

}


void editorInsertChar(int c){
	if(E.cy==E.numrows){
		editorInsertRow(E.numrows,"", 0);
	}
	editorRowInsertChar(&E.row[E.cy], E.cx, c);
	E.cx++;
}

void editorInsertNewLine(){
	if(E.cx==0)
		editorInsertRow(E.cy, "", 0);
	else{
		erow *row =&E.row[E.cy];
		editorInsertRow(E.cy+1, &row->chars[E.cx], row->size-E.cx),
		row=&E.row[E.cy];
		row->size=E.cx;
		row->chars[row->size]='\0';
		editorUpdateRow(row);
	}
	E.cy++;
	E.cx=0;
}

void editorDelChar(){
	if(E.cy==E.numrows)
		return;
	if(E.cx==0 && E.cy==0)
		return;

	erow *row = &E.row[E.cy];
	if(E.cx>0){
		editorRowDelChar(row, E.cx-1);
		E.cx--;
	}else{
		E.cx=E.row[E.cy-1].size;
		editorRowAppendString(&E.row[E.cy-1], row->chars, row->size);
		editorDelRow(E.cy);
		E.cy--;
	}
}

/*** fileI/O ***/

void *editorRowsToString(int *buflen){
	int totlen=0;
	int j;
	for(j=0;j<E.numrows;j++)
		totlen +=E.row[j].size+1;
	*buflen=totlen;

	char *buf=malloc(totlen);
	char *p=buf;
	for(j=0;j<E.numrows;j++){
		memcpy(p, E.row[j].chars, E.row[j].size);
		p+=E.row[j].size;
		*p='\n';
		p++;
	}

	return buf;
}

int numDigits(int n) {
    int count = 0;
    do {
        count++;
        n /= 10;
    } while (n != 0);
    return count;
}


void editorOpen(char *filename){
	free(E.filename);
  	E.filename = strdup(filename);

	editorSelectSyntaxHighlight();

	FILE *fp = fopen(filename, "r");
	if(!fp)
		die("fopen");
	char *line = NULL;
	size_t linecap = 0;
	ssize_t linelen;
	
	while((linelen = getline(&line, &linecap, fp)) != -1){
		while(linelen > 0 && (line[linelen-1] == '\n'|| line[linelen-1] == '\r'))
			linelen--;

		editorInsertRow(E.numrows, line, linelen);	
	}

	free(line);
	fclose(fp);
	E.rowPadding=numDigits(E.numrows);
	E.screencols-=E.rowPadding;
	E.dirty=0;
}

void editorSave(){
	if(E.filename==NULL){
		E.filename=editorPrompt("Save as: %s (ESC to cancel)", NULL);
		if(E.filename==NULL){
			editorSetStatusMessage("Save aborted");
			return;
		}
		editorSelectSyntaxHighlight();
	}
	
	int len;
	char *buf=editorRowsToString(&len);

	int fd=open(E.filename, O_RDWR|O_CREAT, 0644);
	if(fd != -1){
		if(ftruncate(fd, len)!=-1){
			if(write(fd, buf, len)==len){
				close(fd);
				free(buf);
				E.dirty=0;
				editorSetStatusMessage("%d bytes written to disk", len);
				return;
			}
		}
		close(fd);
	}
	free(buf);
	editorSetStatusMessage("Can't save! I/O error: %s", strerror(errno));
}

/*** find ***/

void editorFindCallback(char *query, int key){
	static int last_match = -1;
	static int direction = 1;

	static int saved_hl_line;
	static char *saved_hl = NULL;

	if(saved_hl){
		memcpy(E.row[saved_hl_line].hl, saved_hl, E.row[saved_hl_line].rsize);
		free(saved_hl);
		saved_hl=NULL;
	}

	if(key =='\r' || key == '\x1b'){
		last_match = -1;
		direction = 1;
		return;	
	}else if(key == ARROW_RIGHT || key == ARROW_DOWN){
		direction = 1;
	}else if(key == ARROW_LEFT || key == ARROW_UP){
		direction = -1;
	}else{
		last_match=-1;
		direction=1;
	}	

	if(last_match==-1) 
		direction = 1;
	int current = last_match;
	int i;
	for(i = 0; i < E.numrows; i++){
		current += direction;
		if(current==-1)
			current=E.numrows-1;
		else
			if(current == E.numrows)
				current = 0;
		erow *row= &E.row[current];
		char *match =strstr(row->render,query);
		if(match){
			last_match=current;
			E.cy=current;
			E.cx=editorRowRxToCx(row, match - row->render);
			E.rowoff=E.numrows;
			
			saved_hl_line=current;
			saved_hl=malloc(row->rsize);
			memcpy(saved_hl, row->hl, row->rsize);
			memset(&row->hl[match - row->render], HL_MATCH, strlen(query));
			break;
		}
	}
}

void editorFind(){
	int saved_cx = E.cx;
	int saved_cy = E.cy;
	int saved_coloff = E.coloff;
	int saved_rowoff = E.rowoff;
	
	char *query = editorPrompt("Search: %s (ESC/Arrows/Enter to cancel)", editorFindCallback);
	if(query)
		free(query);
	else{
		E.cx = saved_cx;
		E.cy = saved_cy;
		E.coloff = saved_coloff;
		E.rowoff = saved_rowoff;
	}
}
/*** append buffer ***/
/* We define a very simple "append buffer" structure, that is an heap
 * allocated string where we can append to. This is useful in order to
 * write all the escape sequences in a buffer and flush them to the standard
 * output in a single call, to avoid flickering effects. */
struct abuf{
	char *b;
	int len;
};

#define ABUF_INIT {NULL, 0}

void abAppend(struct abuf *ab, const char *s, int len){
	
	char *new = realloc(ab->b, ab->len + len);
	
	if(new == NULL)
		return;

	memcpy(&new[ab->len], s, len);
	ab->b=new;
	ab->len +=len;
}

void abFree(struct abuf *ab){
	free(ab->b);
}

/*** output ***/

void editorScroll(){
	E.rx = E.cx+E.rowPadding;
	if(E.cy<E.numrows){
		E.rx= editorRowCxToRx(&E.row[E.cy],E.cx);
	}

	if(E.cy<E.rowoff){
		E.rowoff=E.cy;
	}
	if(E.cy >= E.rowoff+E.screenrows){
		E.rowoff=E.cy-E.screenrows+1;
	}
	if(E.rx<E.coloff){
		E.coloff=E.rx;
	}
	if(E.rx >= E.coloff+E.screencols){
		E.coloff=E.rx-E.screencols+1;
	}
}

void  editorDrawRows(struct abuf *ab){
	int y;
	for(y=0;y<E.screenrows;y++){
		int filerow=y+E.rowoff;
		int clen;
		char buf[32];
		clen=snprintf(buf,sizeof(buf),"\x1b[1;32;40m%*d\x1b[m",E.rowPadding, filerow);
		abAppend(ab, buf, clen);
		if(filerow >= E.numrows){
			if(E.numrows == 0 && y == E.screenrows/3){
				char welcome [80];
				int welcomelen =snprintf(welcome, sizeof(welcome), "kilo editor --version %s", KILO_VERSION);
			
				if(welcomelen > E.screencols)
					welcomelen = E.screencols;
			
				int padding = (E.screencols - welcomelen)/2;
				if(padding){
					abAppend(ab, "~", 1);
					padding--;
				}
				while(padding--)
					abAppend(ab, " ", 1);

				abAppend(ab, welcome, welcomelen);
			}else{
				abAppend(ab, "~", 1);
			}
		}else{

			int len = E.row[filerow].rsize - E.coloff;
			if(len<0) 
				len=0;
			if(len > E.screencols)
				len = E.screencols;
			char *c=&E.row[filerow].render[E.coloff];
			unsigned char *hl =&E.row[filerow].hl[E.coloff];
			int current_color =-1;
			int j; 
			for(j=0;j<len; j++){
				if(iscntrl(c[j])){
					char sym = (c[j] <= 26) ? '@' + c[j] : '?';
					abAppend(ab, "\x1b[7m", 4);
					abAppend(ab, &sym, 1);
					abAppend(ab, "\x1b[m", 3);
					if(current_color !=-1){
						clen = snprintf(buf, sizeof(buf), "\x1b[%dm", current_color);
						abAppend(ab, buf, clen);
					}
				}else if(hl[j] == HL_NORMAL){
					if(current_color!=-1){
						abAppend(ab, "\x1b[39m", 5);
						current_color=-1;
					}
					abAppend(ab, &c[j], 1);
				}else{
					int color = editorSyntaxToColor(hl[j]);
					if(color!=current_color){
						current_color=color;
						clen = snprintf(buf, sizeof(buf), "\x1b[%dm", color);
						abAppend(ab, buf, clen);
					}
					abAppend(ab, &c[j], 1);
				}

			}
			abAppend(ab, "\x1b[39m", 5);
		}
	
		abAppend(ab, "\x1b[K", 3);	
		abAppend(ab, "\r\n", 2);
	} 
}

void editorDrawStatusBar(struct abuf *ab){
	abAppend(ab,"\x1b[7m", 4);
	char status[80],rstatus[80];
	int len=snprintf(status, 
					sizeof(status),
					"%d %.20s %s", 
					E.numrows,
					E.filename ? E.filename : "[No Name]",
					E.dirty ? "(modified)" : "");
	int rlen = snprintf( rstatus, 
						sizeof(rstatus),
						"[%s] %s | %d/%d",
						nameMode[E.mode],
						E.syntax ? E.syntax->filetype : "no ft",
						E.cy+1,
						E.numrows);
	if(len>E.screencols+E.rowPadding) 
		len = E.screencols+E.rowPadding;
	abAppend(ab, status, len);
	while(len<E.screencols+E.rowPadding){
		if(E.screencols+E.rowPadding-len==rlen){
			abAppend(ab, rstatus, rlen);
			break;
		}else{
			abAppend(ab, " ", 1);
			len++;
		}
	}
	abAppend(ab,"\x1b[m",3);
	abAppend(ab, "\r\n", 2);
}

void editorDrawMessageBar(struct abuf *ab){
	abAppend(ab, "\x1b[K", 3);
	int msglen=strlen(E.statusmsg);
	if(msglen>E.screencols)
		msglen=E.screencols;
	if(msglen && time(NULL) -E.statusmsg_time < 5)
		abAppend(ab, E.statusmsg, msglen);	
}

void editorRefreshScreen(){
	editorScroll();

	struct abuf ab = ABUF_INIT;

	//Hide the cursor
	abAppend(&ab,"\x1b[?25l", 4);
	//Move the cursor home
	abAppend(&ab,"\x1b[H", 3);

	editorDrawRows(&ab);
	editorDrawStatusBar(&ab);
	editorDrawMessageBar(&ab);

	//Move the cursor
	char buf[32];
	snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (E.cy-E.rowoff) +1 , (E.rx-E.coloff)+1);
	abAppend(&ab, buf, strlen(buf));

	//Show the cursor
	abAppend(&ab,"\x1b[?25h", 4);

	write(STDOUT_FILENO, ab.b ,ab.len);
	abFree(&ab);
}

void editorSetStatusMessage(const char *fmt, ...){
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);
	va_end(ap);
	E.statusmsg_time=time(NULL);
}

/*** input ***/

char *editorPrompt(char *prompt, void (*callback)(char *, int)){
	size_t bufsize=128;
	char *buf=malloc(bufsize);

	size_t buflen=0;
	buf[0]='\0';

	while(1){
		editorSetStatusMessage(prompt, buf);
		editorRefreshScreen();

		int c=editorReadKey();
		if(c==DEL_KEY || c==CTRL_KEY('h') || c==BACKSPACE){
			if(buflen!=0)
				buf[--buflen]='\0';
		}else if( c=='\x1b'){
			editorSetStatusMessage("");
			if(callback)
				callback(buf,c);
			free(buf);
			return NULL;
		}else if(c=='\r'){
			if(buflen != 0){
				editorSetStatusMessage("");
				if(callback)
					callback(buf, c);
				return buf;
			}
		}else if(!iscntrl(c) && c < 128){
				if(buflen == bufsize-1){
					bufsize*=2;
					buf=realloc(buf,bufsize);
				}
				buf[buflen++]=c;
				buf[buflen]='\0';
			}

			if(callback)
				callback(buf, c);
	}
}

void editorMoveCursor(int key){
	erow *row=(E.cy>=E.numrows) ? NULL : &E.row[E.cy];

	switch(key){
		case ARROW_LEFT:
			if(E.cx !=0)
				E.cx--;
			else{
				if(E.cy>0){
					E.cy--;
					E.cx = E.row[E.cy].size;
				}
			}
				
			break;
		case ARROW_RIGHT:
			if(row && E.cx < row->size)
				E.cx++;
			else{
				if(row && E.cx == row->size){
					E.cy++;
					E.cx=0;
				}
			}
			break;
		case ARROW_UP:
			if(E.cy != 0)
				E.cy--;
			break;
		case ARROW_DOWN:
			if(E.cy<E.numrows)
				E.cy++;
			break;
	}

	
	row=(E.cy>=E.numrows) ? NULL : &E.row[E.cy];
	int rowlen = row ? row -> size  : 0;
	if(E.cx > rowlen){
		E.cx = rowlen;
	}
}

//Accept only ARROW_RIGHT or ARROW_LEFT
void editorMoveCursorSkippingWords(int key){
	if(key!=ARROW_RIGHT && key!=ARROW_LEFT)
		return;

	int inword=0;

	editorMoveCursor(key);
	// We want to stop after a word or at the end of the line
	while( (E.cx+E.coloff)&& !(E.row[E.cy].chars[E.coloff+E.cx]=='\0') && (!inword || !is_separator(E.row[E.cy].chars[E.coloff+E.cx]))){
		inword=!is_separator(E.row[E.cy].render[E.coloff+E.cx]);
		editorMoveCursor(key); 
	}
	/*If we are going left the cursor will stop at the caracther before the word
		we want the cursor at the first caracther of the word so we move the cursor right one time*/ 
	if( !(E.row[E.cy].chars[E.coloff+E.cx]=='\0' && !inword) && key==ARROW_LEFT)
		editorMoveCursor(ARROW_RIGHT);
}

/*** VIM MODE ***/

void editorProcessKeypressInsertMode(){

	int c = editorReadKey();

	switch (c){
		case '\r':
			editorInsertNewLine();
			break;
		
		case ESC:
			E.mode=NORMAL_MODE;
			break;
		
		case CTRL_KEY('Q'):
			if(E.dirty && E.quitTimes >0){
				editorSetStatusMessage("WARNING!!! File has unsaved changes. " "Press Ctrl-Q %d more times to quit.", E.quitTimes);
				E.quitTimes--;
				return;
			}

			write(STDOUT_FILENO,"\x1b[2J", 4);
			write(STDOUT_FILENO,"\x1b[H", 3);
			exit(0);
			break;
		
		case CTRL_KEY('s'):
			editorSave();
			break;

		case HOME_KEY:
			E.cx = 0;
			break;
		
		case END_KEY:
			if(E.cy<E.numrows)
				E.cx = E.row[E.cy].size;
			break;

		case CTRL_KEY('f'):
			editorFind();
			break;

		case BACKSPACE:
		case CTRL_KEY('k'):
		case DEL_KEY:
			if(c==DEL_KEY)
				editorMoveCursor(ARROW_RIGHT);
			editorDelChar();
			break;

		case PAGE_UP:
		case PAGE_DOWN:
			{
				if(c==PAGE_UP){
					E.cy=E.rowoff;
				}else{
					if(c== PAGE_DOWN){
						E.cy=E.rowoff+E.screenrows-1;
						if(E.cy>E.numrows)
							E.cy=E.numrows;
					}
				}

				int times = E.screenrows;
				while(times--)
					editorMoveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
			}
			break;

		case CTRL_ARROW_LEFT:
		case CTRL_ARROW_RIGHT:
				c--; //CTRL_ARROW_RIGHT/LEFT-1 == ARROW_RIGHT/LEFT
				editorMoveCursorSkippingWords(c);
			break;
		case ARROW_UP:
		case ARROW_DOWN:
		case ARROW_LEFT:
		case ARROW_RIGHT:
			editorMoveCursor(c);
			break;
		
		case CTRL_KEY('l'):
			break;

		default:
			editorInsertChar(c);
			break;
	}

	E.quitTimes = KILO_QUIT_TIMES;
}

enum comand{
	NONE = 0,
	COPY = 'y',
	DELETE = 'd',
	INSERT = 'i',
	APPEND = 'a',
	APPENDBELOW = 'o',
	VISUAL = 'v'
};

void handleComonComand(int comand){
	switch (comand) {
		//exit
		case CTRL_KEY('Q'):
			if(E.dirty && E.quitTimes >0){
				editorSetStatusMessage("WARNING!!! File has unsaved changes. " "Press Ctrl-Q %d more times to quit.", E.quitTimes);
				E.quitTimes--;
				return;
			}

			write(STDOUT_FILENO,"\x1b[2J", 4);
			write(STDOUT_FILENO,"\x1b[H", 3);
			exit(0);
			break;
		//save
		case CTRL_KEY('s'):
			editorSave();
			break;
	}
}

void handleCommand(int comand, int movement, int direction){
	switch (comand) {
		case NONE:
			if(!movement)
				return;
			if(direction==CTRL_ARROW_LEFT || direction==CTRL_ARROW_RIGHT){
				direction--;
				while(movement--)
					editorMoveCursorSkippingWords(direction);
			}else{
				while(movement--)
					editorMoveCursor(direction);
			}
			break;
		case COPY:
			break;
		case DELETE:
			if(movement){
				switch (direction){
					case ARROW_RIGHT:
						while (movement--){
							editorMoveCursor(ARROW_RIGHT);
							editorDelChar();
						}
						break;
					case ARROW_LEFT:
						while (movement--){
							editorDelChar();
						}
						break;
					case ARROW_DOWN:
						while (movement--){
							editorDelRow(E.cy);
						}
						break;
					case ARROW_UP:
						while (movement--){
							E.cy--;
							editorDelRow(E.cy);
						}
						break;
				}
			}else
				editorDelRow(E.cy);
			break;
		default:
			break;
	}
}

void editorProcessKeypressNormalMode(){
	int c = editorReadKey();
	static int movement;
	static int comand;
	

	handleComonComand(c);
	// Comand key
	switch (c){
		//cange mode
		case ESC:
			comand=NONE;
			movement=0;
			return;

		case INSERT:
			E.mode=INSERT_MODE;
			return;

		case APPEND:
			editorMoveCursor(ARROW_RIGHT);
			E.mode=INSERT_MODE;
			return;

		case APPENDBELOW:
			editorMoveCursor(ARROW_UP);
			editorInsertNewLine();
			editorMoveCursor(ARROW_DOWN);
			break;

		case VISUAL:
			E.mode=VISUAL_MODE;
			return;
		
		//comand normal mode
		case DELETE:
			if(comand==DELETE){
				//delete the row
				handleCommand(DELETE,0,0);
				comand=NONE;
				movement=0;
			}else{
				comand=DELETE;
				movement=0;
				goto print;
			}
			break;
		case COPY:
			if(comand==COPY)
				//TODO
			comand=COPY;
			movement=0;
			goto print;
	}

	// Movement
	if(c>='0' && c<='9'){
		c-='0';
		movement*=10;
		movement+=c;
		goto print;
	}

	// Direction
	switch (c){
		// ARROW_LEFT
		case BACKSPACE:
		case 'h':
			c=ARROW_LEFT;
			break;

		// ARROW_DOWN:
		case ENTER:
		case 'j':
			c=ARROW_DOWN;
			break;

		// ARROW_UP:
		case 'k':
			c=ARROW_UP;
			break;

		// ARROW_RIGHT:
		case 'l':
			c=ARROW_RIGHT;
			break;

		// skip words ARROW_RIGHT
		case 'w':
			c=CTRL_ARROW_RIGHT;
			break;

		// skip words ARROW_LEFT
		case 'b':
			c=CTRL_ARROW_LEFT;
			break;
		default:
			if(c!=ARROW_LEFT && c!=ARROW_RIGHT && c!=ARROW_UP && c!=ARROW_DOWN)
				comand=NONE;
	}
	handleCommand(comand, (!movement)?1:movement, c);
	comand=NONE;
	movement=0;
	return;

	print:
	if(movement)
		editorSetStatusMessage("%c%d",(!comand)?' ':comand,movement);
	else
		editorSetStatusMessage("%c",comand);
}

void editorProcessKeypressVisualMode(){
	int c = editorReadKey();
	static int startCx;
	static int startCy;
	static int in; //trak if i was already in visualmode or if is the first time I get in
	static int comand;
	static int movement;
	
	if(startCx&&startCy){}

	handleComonComand(c);
	// Comand key
	switch (c){
		//cange mode
		case ESC:
			comand=NONE;
			movement=0;
			return;
		
		case DELETE:
			comand=DELETE;
			movement=0;
			goto print;
			break;
		case COPY:
			if(comand==COPY)
				//TODO
			comand=COPY;
			movement=0;
			goto print;
	}

	// Movement
	if(c>='0' && c<='9'){
		c-='0';
		movement*=10;
		movement+=c;
		goto print;
	}

	// Direction
	switch (c){
		// ARROW_LEFT
		case BACKSPACE:
		case 'h':
			c=ARROW_LEFT;
			break;

		// ARROW_DOWN:
		case ENTER:
		case 'j':
			c=ARROW_DOWN;
			break;

		// ARROW_UP:
		case 'k':
			c=ARROW_UP;
			break;

		// ARROW_RIGHT:
		case 'l':
			c=ARROW_RIGHT;
			break;

		// skip words ARROW_RIGHT
		case 'w':
			c=CTRL_ARROW_RIGHT;
			break;

		// skip words ARROW_LEFT
		case 'b':
			c=CTRL_ARROW_LEFT;
			break;
		default:
			if(c!=ARROW_LEFT && c!=ARROW_RIGHT && c!=ARROW_UP && c!=ARROW_DOWN)
				comand=NONE;
	}
	handleCommand(comand, (!movement)?1:movement, c);
	comand=NONE;
	movement=0;
	return;

	print:
	if(movement)
		editorSetStatusMessage("%c%d",comand,movement);
	else
		editorSetStatusMessage("%c",comand);
}


/*** init ***/

void initEditor(){
	E.cx=0;
	E.cy=0;
	E.rx=0;
	E.rowoff=0;
	E.coloff=0;
	E.numrows=0;
	E.row = NULL;
	E.dirty=0;
	E.filename=NULL;
	E.statusmsg[0]='\0';
	E.statusmsg_time=0;
	E.syntax=NULL;
	E.mode=NORMAL_MODE;
	E.quitTimes= KILO_QUIT_TIMES;
	E.rowPadding=0;
  E.startVisualModeCy=-1;
  E.startVisualModeCx=-1;
  E.copyBuffer=nullptr;

	if(getWindowSize(&E.screenrows, &E.screencols) == -1)
		die("getWindowSize");
	E.screenrows-=2;
}



int main(int argc, char * argv[]){
	enableRawMode();
	initEditor();
	if(argc >= 2)
		editorOpen(argv[1]);

	editorSetStatusMessage("HELP: Ctrl-S = save | Ctrl-Q = quit | Ctrl-F = find");

	while (1){
		editorRefreshScreen();
		switch (E.mode){
			case NORMAL_MODE:
				editorProcessKeypressNormalMode();
				break;
			case INSERT_MODE:
				editorProcessKeypressInsertMode();
				break;
			case VISUAL_MODE:
				editorProcessKeypressVisualMode();
				break;
		}
	}
	return 0;
}
