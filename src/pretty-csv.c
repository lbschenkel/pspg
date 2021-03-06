/*-------------------------------------------------------------------------
 *
 * pretty-csv.c
 *	  import and formatting csv documents
 *
 * Portions Copyright (c) 2017-2019 Pavel Stehule
 *
 * IDENTIFICATION
 *	  src/pretty-csv.c
 *
 *-------------------------------------------------------------------------
 */
#include <ctype.h>
#include <libgen.h>
#include <locale.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "pspg.h"
#include "unicode.h"

#ifndef offsetof
#define offsetof(type, field)	((long) &((type *)0)->field)
#endif							/* offsetof */

typedef struct
{
	char	   *buffer;
	int			processed;
	int			used;
	int			size;
	int			maxfields;
	int			starts[1024];		/* start of first char of column (in bytes) */
	int			sizes[1024];		/* lenght of chars of column (in bytes) */
	long int	digits[1024];		/* number of digits, used for format detection */
	long int	tsizes[1024];		/* size of column in bytes, used for format detection */
	int			firstdigit[1024];	/* rows where first char is digit */
	int			widths[1024];			/* column's display width */
	bool		multilines[1024];		/* true if column has multiline row */
} LinebufType;

typedef struct
{
	char	   *buffer;
	int			used;
	int			size;
	int			free;
	LineBuffer *linebuf;
	bool		force8bit;
	int			flushed_rows;		/* number of flushed rows */
	int			maxbytes;
	bool		printed_headline;
} PrintbufType;

typedef struct
{
	int			border;
	char		linestyle;
	bool		double_header;
} PrintConfigType;

static void *
smalloc(int size, char *debugstr)
{
	char *result;

	result = malloc(size);
	if (!result)
	{
		if (debugstr)
			fprintf(stderr, "out of memory while %s\n", debugstr);
		else
			fprintf(stderr, "out of memory\n");
		exit(EXIT_FAILURE);
	}

	return result;
}

/*
 * Add new row to LineBuffer
 */
static void
pb_flush_line(PrintbufType *printbuf)
{
	char	   *line;

	if (printbuf->linebuf->nrows == 1000)
	{
		LineBuffer *nb = smalloc(sizeof(LineBuffer), "serialize csv output");

		memset(nb, 0, sizeof(LineBuffer));

		printbuf->linebuf->next = nb;
		nb->prev = printbuf->linebuf;
		printbuf->linebuf = nb;
	}

	line = smalloc(printbuf->used + 1, "serialize csv output");
	memcpy(line, printbuf->buffer, printbuf->used);
	line[printbuf->used] = '\0';

	printbuf->linebuf->rows[printbuf->linebuf->nrows++] = line;

	if (printbuf->used > printbuf->maxbytes)
		printbuf->maxbytes = printbuf->used;

	printbuf->used = 0;
	printbuf->free = printbuf->size;

	printbuf->flushed_rows += 1;
}

static void
pb_write(PrintbufType *printbuf, const char *str, int size)
{
	if (printbuf->free < size)
	{
		printbuf->size += 10 * 1024;
		printbuf->free += 10 * 1024;

		printbuf->buffer = realloc(printbuf->buffer, printbuf->size);
		if (!printbuf->buffer)
		{
			fprintf(stderr, "out of memory while serialize csv output\n");
			exit(EXIT_FAILURE);
		}
	}

	memcpy(printbuf->buffer + printbuf->used, str, size);
	printbuf->used += size;
	printbuf->free -= size;
}

static void
pb_writes(PrintbufType *printbuf, const char *str)
{
	pb_write(printbuf, str, strlen(str));
}


static void
pb_write_repeat(PrintbufType *printbuf, int n, const char *str, int size)
{
	bool	need_realloc = false;

	while (printbuf->free < size * n)
	{
		printbuf->size += 10 * 1024;
		printbuf->free += 10 * 1024;

		need_realloc = true;
	}

	if (need_realloc)
	{
		printbuf->buffer = realloc(printbuf->buffer, printbuf->size);
		if (!printbuf->buffer)
		{
			fprintf(stderr, "out of memory while serialize csv output\n");
			exit(EXIT_FAILURE);
		}
	}

	while (n--)
	{
		memcpy(printbuf->buffer + printbuf->used, str, size);
		printbuf->used += size;
		printbuf->free -= size;
	}
}

static void
pb_writes_repeat(PrintbufType *printbuf, int n,  const char *str)
{
	pb_write_repeat(printbuf, n, str, strlen(str));
}


static void
pb_putc(PrintbufType *printbuf, char c)
{
	if (printbuf->free < 1)
	{
		printbuf->size += 10 * 1024;
		printbuf->free += 10 * 1024;

		printbuf->buffer = realloc(printbuf->buffer, printbuf->size);
		if (!printbuf->buffer)
		{
			fprintf(stderr, "out of memory while serialize csv output\n");
			exit(1);
		}
	}

	printbuf->free -= 1;
	printbuf->buffer[printbuf->used++] = c;
}

static void
pb_puts(PrintbufType *printbuf, char *str)
{
	pb_write(printbuf, str, strlen(str));
}

static void
pb_putc_repeat(PrintbufType *printbuf, int n, int c)
{
	bool	need_realloc = false;

	while (printbuf->free < n)
	{
		printbuf->size += 10 * 1024;
		printbuf->free += 10 * 1024;

		need_realloc = true;
	}

	if (need_realloc)
	{
		printbuf->buffer = realloc(printbuf->buffer, printbuf->size);
		if (!printbuf->buffer)
		{
			fprintf(stderr, "out of memory while serialize csv output\n");
			exit(EXIT_FAILURE);
		}
	}

	memset(printbuf->buffer + printbuf->used, c, n);
	printbuf->used += n;
	printbuf->free -= n;

//	while (n--)
//		printbuf->buffer[printbuf->used++] = c;

	printbuf->free -= n;
}

static void
pb_print_vertical_header(PrintbufType *printbuf, PrintDataDesc *pdesc, PrintConfigType *pconfig, char pos)
{
	int		border = pconfig->border;
	bool	double_header = pconfig->double_header;
	char	linestyle = pconfig->linestyle;

	const char *lhchr;				/* left header char */
	const char *mhchr;				/* middle header char */
	const char *rhchr;				/* right header char */
	const char *hhchr;				/* horizont header char */

	int		i;

	/* leave fast when there is nothing to work */
	if ((border == 0 || border == 1) && (pos != 'm'))
		return;

	if (linestyle == 'a')
	{
		if (pos == 'm' &&double_header)
		{
			lhchr = ":";
			mhchr = ":";
			rhchr = ":";
			hhchr = "=";
		}
		else
		{
			lhchr = "+";
			mhchr = "+";
			rhchr = "+";
			hhchr = "-";
		}
	}
	else
	{
		/* linestyle = 'u' */
		if (pos == 'm')
		{
			if (double_header)
			{
				lhchr = "\342\225\236";		/* ╞ */
				mhchr = "\342\225\252";		/* ╪ */
				rhchr = "\342\225\241";		/* ╡ */
				hhchr = "\342\225\220";		/* ═ */
			}
			else
			{
				lhchr = "\342\224\234";		/* ├ */
				mhchr = "\342\224\274";		/* ┼ */
				rhchr = "\342\224\244";		/* ┤ */
				hhchr = "\342\224\200";		/* ─ */
			}
		}
		else if (pos == 't')
		{
			lhchr = "\342\224\214";		/* ┌ */
			mhchr = "\342\224\254";		/* ┬ */
			rhchr = "\342\224\220";		/* ┐ */
			hhchr = "\342\224\200";		/* ─ */
		}
		else
		{
			/* pos == 'b' */
			lhchr = "\342\224\224";		/* └ */
			mhchr = "\342\224\264";		/* ┴ */
			rhchr = "\342\224\230";		/* ┘ */
			hhchr = "\342\224\200";		/* ─ */
		}
	}

	if (border == 2)
	{
		pb_writes(printbuf, lhchr);
		pb_writes(printbuf, hhchr);
	}
	else if (border == 1)
	{
		pb_writes(printbuf, hhchr);
	}

	for (i = 0; i < pdesc->nfields; i++)
	{
		if (i > 0)
		{
			if (border == 0)
			{
				pb_write(printbuf, " ", 1);
			}
			else
			{
				pb_writes(printbuf, hhchr);
				pb_writes(printbuf, mhchr);
				pb_writes(printbuf, hhchr);
			}
		}

		pb_writes_repeat(printbuf, pdesc->widths[i], hhchr);
	}

	if (border == 2)
	{
		pb_writes(printbuf, hhchr);
		pb_writes(printbuf, rhchr);
	}
	else if (border == 1)
	{
		pb_writes(printbuf, hhchr);
	}
	else if (border == 0 && pdesc->multilines[pdesc->nfields - 1])
	{
			pb_write(printbuf, " ", 1);
	}

	pb_flush_line(printbuf);
}

/*
 * Header detection - simple heuristic, when first row has all text fields
 * and second rows has any numeric field, then csv has header.
 */
static bool
is_header(RowBucketType *rb)
{
	RowType	   *row;
	int		i;

	if (rb->nrows < 2)
		return false;

	row = rb->rows[0];

	for (i = 0; i < row->nfields; i++)
	{
		if (row->fields[i][0] == '\0')
			return false;
		if (isdigit((row->fields[i])[0]))
			return false;
	}

	row = rb->rows[1];

	for (i = 0; i < row->nfields; i++)
	{
		if (row->fields[i][0] == '\0')
			return true;
		if (isdigit((row->fields[i])[0]))
			return true;
	}

	return false;
}

static char *
pb_put_line(char *str, bool multiline, PrintbufType *printbuf)
{
	char   *nextline = NULL;

	if (multiline)
	{
		char   *ptr = str;
		int		size = 0;
		int		chrl;

		while (*ptr)
		{
			if (*ptr == '\n')
			{
				nextline = ptr + 1;
				break;
			}

			chrl = printbuf->force8bit ? 1 : utf8charlen(*ptr);
			size += chrl;
			ptr += chrl;
		}

		pb_write(printbuf, str, size);
	}
	else
		pb_puts(printbuf, str);

	return nextline;
}

/*
 * Print formatted data loaded inside RowBuckets
 */
static void
pb_print_rowbuckets(PrintbufType *printbuf,
				   RowBucketType *rb,
				   PrintConfigType *pconfig,
				   PrintDataDesc *pdesc,
				   char *title)
{
	bool	is_last_column_multiline = pdesc->multilines[pdesc->nfields - 1];
	int		last_column_num = pdesc->nfields - 1;
	int		printed_rows = 0;
	char	linestyle = pconfig->linestyle;
	int		border = pconfig->border;
	char	buffer[20];

	printbuf->printed_headline = false;
	printbuf->flushed_rows = 0;
	printbuf->maxbytes = 0;

	if (title)
	{
		pb_puts(printbuf, title);
		pb_flush_line(printbuf);
	}

	pb_print_vertical_header(printbuf, pdesc, pconfig, 't');

	while (rb)
	{
		int		i;

		for (i = 0; i < rb->nrows; i++)
		{
			int		j;
			bool	isheader = false;
			RowType	   *row;
			bool	free_row;
			bool	more_lines = true;
			bool	multiline = rb->multilines[i];

			/*
			 * For multilines we can modify pointers so do copy now
			 */
			if (multiline)
			{
				RowType	   *source = rb->rows[i];
				int			size;

				size = offsetof(RowType, fields) + (source->nfields * sizeof(char*));

				row = smalloc(size, "RowType");
				memcpy(row, source, size);

				free_row = true;
			}
			else
			{
				row = rb->rows[i];
				free_row = false;
			}

			while (more_lines)
			{
				more_lines = false;

				if (border == 2)
				{
					if (linestyle == 'a')
						pb_write(printbuf, "| ", 2);
					else
						pb_write(printbuf, "\342\224\202 ", 4);
				}
				else if (border == 1)
					pb_write(printbuf, " ", 1);

				isheader = printed_rows == 0 ? pdesc->has_header : false;

				for (j = 0; j < row->nfields; j++)
				{
					int		width;
					int		spaces;
					char   *field;
					bool	_more_lines = false;

					if (j > 0)
					{
						if (border != 0)
						{
							if (linestyle == 'a')
								pb_write(printbuf, "| ", 2);
							else
								pb_write(printbuf, "\342\224\202 ", 4);
						}
					}

					field = row->fields[j];

					if (field && *field != '\0')
					{
						bool	left_align = pdesc->types[j] != 'd';

						if (printbuf->force8bit)
						{
							if (multiline)
							{
								char   *ptr = field;
								width = 0;

								while (*ptr)
								{
									if (*ptr++ == '\n')
									{
										more_lines |= true;
										break;
									}
									width += 1;
								}
							}
							else
								width = strlen(field);
						}
						else
						{
							if (multiline)
							{
								width = utf_string_dsplen_multiline(field, SIZE_MAX, &_more_lines, true, NULL, NULL);
								more_lines |= _more_lines;
							}
							else
								width = utf_string_dsplen(field, SIZE_MAX);
						}

						spaces = pdesc->widths[j] - width;

/* ToDo: bug - wrong calculate width */
						if (spaces < 0)
							spaces = 0;

						/* left spaces */
						if (isheader)
							pb_putc_repeat(printbuf, spaces / 2, ' ');
						else if (!left_align)
							pb_putc_repeat(printbuf, spaces, ' ');

						if (multiline)
							row->fields[j] = pb_put_line(row->fields[j], multiline, printbuf);
						else
							(void) pb_put_line(row->fields[j], multiline, printbuf);

						/* right spaces */
						if (isheader)
							pb_putc_repeat(printbuf, spaces - (spaces / 2), ' ');
						else if (left_align)
							pb_putc_repeat(printbuf, spaces, ' ');
					}
					else
						pb_putc_repeat(printbuf, pdesc->widths[j], ' ');

					if (_more_lines)
					{
						if (linestyle == 'a')
							pb_putc(printbuf, '+');
						else
							pb_write(printbuf, "\342\206\265", 3);
					}
					else
					{
						if (border != 0 || j < last_column_num || is_last_column_multiline)
							pb_putc(printbuf, ' ');
					}
				}

				for (j = row->nfields; j < pdesc->nfields; j++)
				{
					bool	addspace;

					if (j > 0)
					{
						if (border != 0)
						{
							if (linestyle == 'a')
								pb_write(printbuf, "| ", 2);
							else
								pb_write(printbuf, "\342\224\202 ", 4);
						}
					}

					addspace = border != 0 || j < last_column_num || is_last_column_multiline;

					pb_putc_repeat(printbuf, pdesc->widths[j] + (addspace ? 1 : 0), ' ');
				}

				if (border == 2)
				{
					if (linestyle == 'a')
						pb_write(printbuf, "|", 2);
					else
						pb_write(printbuf, "\342\224\202", 3);
				}

				pb_flush_line(printbuf);

				if (isheader)
				{
					pb_print_vertical_header(printbuf, pdesc, pconfig, 'm');
					printbuf->printed_headline = true;
				}

				printed_rows += 1;
			}

			if (free_row)
				free(row);
		}

		rb = rb->next_bucket;
	}

	pb_print_vertical_header(printbuf, pdesc, pconfig, 'b');

	snprintf(buffer, 20, "(%d rows)", printed_rows - (printbuf->printed_headline ? 1 : 0));
	pb_puts(printbuf, buffer);
	pb_flush_line(printbuf);
}

/*
 * Try to detect column type and prepare all data necessary for printing
 */
static void
prepare_pdesc(RowBucketType *rb, LinebufType *linebuf, PrintDataDesc *pdesc)
{
	int				i;

	/* copy data from linebuf */
	pdesc->nfields = linebuf->maxfields;
	pdesc->has_header = is_header(rb);

	for (i = 0; i < pdesc->nfields; i++)
	{
		pdesc->widths[i] = linebuf->widths[i];
		pdesc->multilines[i] = linebuf->multilines[i];
	}


	/* try to detect types from numbers of digits */
	for (i = 0; i < pdesc->nfields; i++)
	{
		if ((linebuf->tsizes[i] == 0 && linebuf->digits[i] > 0) ||
			(linebuf->firstdigit[i] > 0 && linebuf->processed - 1 == 1))
			pdesc->types[i] = 'd';
		else if ((((double) linebuf->firstdigit[i] / (double) (linebuf->processed - 1)) > 0.8)
			&& (((double) linebuf->digits[i] / (double) linebuf->tsizes[i]) > 0.5))
			pdesc->types[i] = 'd';
		else
			pdesc->types[i] = 'a';
	}
}

static void
read_csv(RowBucketType *rb,
		 LinebufType *linebuf,
		 char sep,
		 bool force8bit,
		 FILE *ifile)
{
	bool	skip_initial = true;
	bool	closed = false;
	int		first_nw = 0;
	int		last_nw = 0;
	int		pos = 0;
	int		nfields = 0;
	int		instr = false;			/* true when csv string is processed */
	int		c;

	c = fgetc(ifile);
	do
	{
		if (c != EOF && (c != '\n' || instr))
		{
			int		l;

			if (skip_initial)
			{
				if (c == ' ')
					goto next_char;

				skip_initial = false;
				last_nw = first_nw;
			}

			if (linebuf->used >= linebuf->size)
			{
				linebuf->size += linebuf->size < (10 * 1024) ? linebuf->size  : (10 * 1024);
				linebuf->buffer = realloc(linebuf->buffer, linebuf->size);

				/* for debug purposes */
				memset(linebuf->buffer + linebuf->used, 0, linebuf->size - linebuf->used);
			}

			if (c == '"')
			{
				if (instr)
				{
					int		c2 = fgetc(ifile);

					if (c2 == '"')
					{
						/* double double quotes */
						linebuf->buffer[linebuf->used++] = c;
						pos = pos + 1;
					}
					else
					{
						/* start of end of string */
						ungetc(c2, ifile);
						instr = false;
					}
				}
				else
					instr = true;
			}
			else
			{
				linebuf->buffer[linebuf->used++] = c;
				pos = pos + 1;
			}

			if (sep == -1 && !instr)
			{
				/*
				 * Automatic separator detection - now it is very simple, first win.
				 * Can be enhanced in future by more sofisticated mechanism.
				 */
				if (c == ',')
					sep = ',';
				else if (c == ';')
					sep = ';';
				else if (c == '|')
					sep = '|';
			}

			if (sep != -1 && c == sep && !instr)
			{
				if (nfields >= 1024)
				{
					fprintf(stderr, "too much columns");
					exit(1);
				}

				if (!skip_initial)
				{
					linebuf->sizes[nfields] = last_nw - first_nw;
					linebuf->starts[nfields++] = first_nw;
				}
				else
				{
					linebuf->sizes[nfields] = 0;
					linebuf->starts[nfields++] = -1;
				}

				skip_initial = true;
				first_nw = pos;
			}
			else if (instr || c != ' ')
			{
				last_nw = pos;
			}

			l = force8bit ? 1 : utf8charlen(c);
			if (l > 1)
			{
				int		i;

				/* read othe chars */
				for (i = 1; i < l; i++)
				{
					c = fgetc(ifile);
					if (c == EOF)
					{
						fprintf(stderr, "unexpected quit, broken unicode char\n");
						break;
					}

					linebuf->buffer[linebuf->used++] = c;
					pos = pos + 1;
				}
				last_nw = pos;
			}
		}
		else
		{
			char	   *locbuf;
			RowType	   *row;
			int			i;
			int			data_size;
			bool		multiline;

			if (!skip_initial)
			{
				linebuf->sizes[nfields] = last_nw - first_nw;
				linebuf->starts[nfields++] = first_nw;
			}
			else
			{
				linebuf->sizes[nfields] = 0;
				linebuf->starts[nfields++] = -1;
			}

			/* move row from linebuf to rowbucket */
			if (rb->nrows >= 1000)
			{
				RowBucketType *new = smalloc(sizeof(RowBucketType), "import csv data");

				new->nrows = 0;
				new->allocated = true;
				new->next_bucket = NULL;

				rb->next_bucket = new;
				rb = new;
			}

			if (!linebuf->used)
				goto next_row;

			data_size = 0;
			for (i = 0; i < nfields; i++)
				data_size += linebuf->sizes[i] + 1;

			locbuf = smalloc(data_size, "import csv data");
			memset(locbuf, 0, data_size);

			row = smalloc(offsetof(RowType, fields) + (nfields * sizeof(char*)), "import csv data");
			row->nfields = nfields;

			multiline = false;

			for (i = 0; i < nfields; i++)
			{
				int		width;
				bool	_multiline;
				long int		digits = 0;
				long int		total = 0;

				row->fields[i] = locbuf;

				if (linebuf->sizes[i] > 0)
					memcpy(locbuf, linebuf->buffer + linebuf->starts[i], linebuf->sizes[i]);

				locbuf[linebuf->sizes[i]] = '\0';
				locbuf += linebuf->sizes[i] + 1;

				if (force8bit)
				{
					int		cw = 0;
					char   *ptr = row->fields[i];

					width = 0;

					while (*ptr)
					{
						if (isdigit(*ptr))
							digits += 1;
						else if (*ptr != '-' && *ptr != ' ' && *ptr != ':')
							total += 1;

						if (*ptr++ == '\n')
						{
							_multiline = true;
							width = cw > width ? cw : width;

							cw = 0;
						}
						else
							cw++;
					}

					width = cw > width ? cw : width;
				}
				else
				{
					width = utf_string_dsplen_multiline(row->fields[i], linebuf->sizes[i], &_multiline, false, &digits, &total);
				}

				/* skip first possible header row */
				if (linebuf->processed > 0)
				{
					linebuf->tsizes[i] += total;
					linebuf->digits[i] += digits;

					if (isdigit(*row->fields[i]))
						linebuf->firstdigit[i]++;
				}

				if (width > linebuf->widths[i])
					linebuf->widths[i] = width;

				multiline |= _multiline;
				linebuf->multilines[i] |= _multiline;
			}

			if (nfields > linebuf->maxfields)
				linebuf->maxfields = nfields;

			rb->multilines[rb->nrows] = multiline;
			rb->rows[rb->nrows++] = row;

next_row:

			linebuf->used = 0;
			nfields = 0;

			linebuf->processed += 1;

			skip_initial = true;
			first_nw = 0;
			last_nw = 0;
			pos = 0;

			closed = c == EOF;
		}

next_char:

		c = fgetc(ifile);
	}
	while (!closed);
}

/*
 * Read external unformatted data (csv or result of some query
 *
 */
bool
read_and_format(FILE *fp, Options *opts, DataDesc *desc, const char **err)
{
	LinebufType		linebuf;
	RowBucketType	rowbuckets, *rb;
	PrintConfigType	pconfig;
	PrintbufType	printbuf;
	PrintDataDesc	pdesc;

	memset(desc, 0, sizeof(DataDesc));

	if (!opts->query)
	{
		if (fp != NULL)
		{
			if (opts->pathname != NULL)
			{
				char	   *name;

				name = basename(opts->pathname);
				strncpy(desc->filename, name, 64);
				desc->filename[64] = '\0';
			}
		}
		else
			fp = stdin;
	}

	desc->title[0] = '\0';
	desc->title_rows = 0;
	desc->border_top_row = -1;
	desc->border_head_row = -1;
	desc->border_bottom_row = -1;
	desc->first_data_row = -1;
	desc->last_data_row = -1;
	desc->is_expanded_mode = false;
	desc->headline_transl = NULL;
	desc->cranges = NULL;
	desc->columns = 0;
	desc->footer_row = -1;
	desc->alt_footer_row = -1;
	desc->is_pgcli_fmt = false;
	desc->namesline = NULL;
	desc->order_map = NULL;
	desc->total_rows = 0;
	desc->multilines_already_tested = false;

	desc->maxbytes = -1;
	desc->maxx = -1;

	memset(&desc->rows, 0, sizeof(LineBuffer));
	desc->rows.prev = NULL;

	memset(&linebuf, 0, sizeof(LinebufType));

	linebuf.buffer = malloc(10 * 1024);
	linebuf.used = 0;
	linebuf.size = 10 * 1024;

	pconfig.linestyle = (opts->force_ascii_art || opts->force8bit) ? 'a' : 'u';
	pconfig.border = opts->border_type;
	pconfig.double_header = opts->double_header;

	rowbuckets.allocated = false;

	if (opts->query)
	{
		if (!pg_exec_query(opts, &rowbuckets, &pdesc, err))
			return false;
	}
	else
	{
		read_csv(&rowbuckets, &linebuf, opts->csv_separator, opts->force8bit, fp);
		prepare_pdesc(&rowbuckets, &linebuf, &pdesc);
	}

	/* reuse allocated memory */
	printbuf.buffer = linebuf.buffer;
	printbuf.size = linebuf.size;
	printbuf.free = linebuf.size;
	printbuf.used = 0;
	printbuf.linebuf = &desc->rows;
	printbuf.force8bit = opts->force8bit;

	/* init other printbuf fields */
	printbuf.printed_headline = false;
	printbuf.flushed_rows = 0;
	printbuf.maxbytes = 0;

	/* sanitize ptr */
	linebuf.buffer = NULL;
	linebuf.size = 0;

	pb_print_rowbuckets(&printbuf, &rowbuckets, &pconfig, &pdesc, NULL);

	desc->border_type = pconfig.border;
	desc->linestyle = pconfig.linestyle;
	desc->maxbytes = printbuf.maxbytes;

	if (printbuf.printed_headline)
	{
		int		headline_rowno;

		headline_rowno = pconfig.border == 2 ? 2 : 1;

		if (desc->rows.nrows > headline_rowno)
		{
			desc->namesline = desc->rows.rows[headline_rowno - 1];

			desc->border_head_row = headline_rowno;
			desc->headline = desc->rows.rows[headline_rowno];
			desc->headline_size = strlen(desc->headline);

			if (opts->force8bit)
				desc->headline_char_size = desc->headline_size;
			else
				desc->headline_char_size = desc->maxx = utf_string_dsplen(desc->headline, SIZE_MAX);

			desc->first_data_row = desc->border_head_row + 1;

			desc->maxy = printbuf.flushed_rows - 1;
			desc->total_rows = printbuf.flushed_rows;
			desc->last_row = desc->total_rows - 1;

			desc->footer_row = desc->last_row;
			desc->footer_rows = 1;

			if (pconfig.border == 2)
			{
				desc->border_top_row = 0;
				desc->last_data_row = desc->total_rows - 2 - 1;
				desc->border_bottom_row = desc->last_data_row + 1;
			}
			else
			{
				desc->border_top_row = -1;
				desc->border_bottom_row = -1;
				desc->last_data_row = desc->total_rows - 1 - 1;
			}
		}
	}
	else
	{
		char	*ptr;
		int		i;

		/*
		 * When we have not headline. We know structure, so we can
		 * "translate" headline here (generate translated headline).
		 */
		desc->columns = linebuf.maxfields;
		desc->cranges = smalloc(desc->columns * sizeof(CRange), "prepare metadata");
		memset(desc->cranges, 0, desc->columns * sizeof(CRange));
		desc->headline_transl = smalloc(desc->maxbytes + 3, "prepare metadata");

		ptr = desc->headline_transl;

		if (pconfig.border == 1)
			*ptr++ = 'd';
		else if (pconfig.border == 2)
		{
			*ptr++ = 'L';
			*ptr++ = 'd';
		}

		for (i = 0; i < linebuf.maxfields; i++)
		{
			int		width = linebuf.widths[i];

			desc->cranges[i].name_pos = -1;
			desc->cranges[i].name_size = -1;

			if (i > 0)
			{
				if (pconfig.border > 0)
				{
					*ptr++ = 'd';
					*ptr++ = 'I';
					*ptr++ = 'd';
				}
				else
					*ptr++ = 'I';
			}

			while (width--)
			{
				*ptr++ = 'd';
			}
		}

		if (pconfig.border == 1)
			*ptr++ = 'd';
		else if (pconfig.border == 2)
		{
			*ptr++ = 'd';
			*ptr++ = 'R';
		}

		*ptr = '\0';
		desc->headline_char_size = strlen(desc->headline_transl);

		desc->cranges[0].xmin = 0;
		ptr = desc->headline_transl;
		i = 0;

		while (*ptr)
		{
			if (*ptr++ == 'I')
			{
				desc->cranges[i].xmax = ptr - desc->headline_transl - 1;
				desc->cranges[++i].xmin = ptr - desc->headline_transl - 1;
			}
		}

		desc->cranges[i].xmax = desc->headline_char_size - 1;

		desc->maxy = printbuf.flushed_rows - 1;
		desc->total_rows = printbuf.flushed_rows;
		desc->last_row = desc->total_rows - 1;

		desc->footer_row = desc->last_row;
		desc->footer_rows = 1;

		if (pconfig.border == 2)
		{
			desc->first_data_row = 0;
			desc->border_top_row = 0;
			desc->border_head_row = 0;
			desc->last_data_row = desc->total_rows - 2 - 1;
			desc->border_bottom_row = desc->last_data_row + 1;
		}
		else
		{
			desc->first_data_row = 0;
			desc->border_top_row = -1;
			desc->border_head_row = -1;

			desc->border_bottom_row = -1;
			desc->last_data_row = desc->total_rows - 1 - 1;
		}
	}

	free(printbuf.buffer);

	/* release row buckets */
	rb = &rowbuckets;

	while (rb)
	{
		RowBucketType	*nextrb;
		int		i;

		for (i = 0; i < rb->nrows; i++)
		{
			RowType	   *r = rb->rows[i];

			/* only first field holds allocated string */
			if (r->nfields > 0)
				free(r->fields[0]);
			free(r);
		}

		nextrb = rb->next_bucket;
		if (rb->allocated)
			free(rb);
		rb = nextrb;
	}

	*err = NULL;

	return true;
}
