/* Small Python lexer for display only; not a parser or validator. */
struct python_state {
	char quote;
};

static int py_identifier(unsigned char c) {

	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c >= 128;
}

static int py_keyword(const char *text, size_t size) {

	static const char *words[] = {"False", "None", "True", "and", "as", "assert", "async", "await",
		"break", "class", "continue", "def", "del", "elif", "else", "except", "finally", "for",
		"from", "global", "if", "import", "in", "is", "lambda", "nonlocal", "not", "or", "pass",
		"raise", "return", "try", "while", "with", "yield", "match", "case"};
	for (size_t i = 0; i < sizeof(words) / sizeof(*words); i++) {
		if (strlen(words[i]) == size && !memcmp(text, words[i], size)) {
			return 1;
		}
	}

	return 0;
}

/* Pairs: keyword, string, number, comment, function/class name. */
static void md_python(struct md_canvas *c, const char *text, size_t size, int background,
	struct python_state *state) {

	int base = background == 6 ? 14 : 19;
	int plain = background == 6 ? 6 : 24;
	size_t at = 0;
	int definition = 0;
	while (at < size) {
		size_t start = at;
		int pair = plain;
		if (state->quote || text[at] == '\'' || text[at] == '"') {
			char quote = state->quote ? state->quote : text[at];
			int triple =
				state->quote || (at + 2 < size && text[at + 1] == quote && text[at + 2] == quote);
			if (!state->quote) {
				at += triple ? 3 : 1;
			}
			state->quote = triple ? quote : 0;
			while (at < size) {
				if (text[at] == '\\') {
					at += at + 1 < size ? 2 : 1;
					continue;
				}
				if (text[at] == quote &&
					(!triple ||
						(at + 2 < size && text[at + 1] == quote && text[at + 2] == quote))) {
					at += triple ? 3 : 1;
					state->quote = 0;
					break;
				}
				at++;
			}
			pair = base + 1;
		} else if (text[at] == '#') {
			at = size;
			pair = base + 3;
		} else if (text[at] >= '0' && text[at] <= '9') {
			at++;
			while (at < size &&
				(py_identifier((unsigned char)text[at]) || (text[at] >= '0' && text[at] <= '9') ||
					text[at] == '.' ||
					((text[at] == '+' || text[at] == '-') && at &&
						(text[at - 1] == 'e' || text[at - 1] == 'E')))) {
				at++;
			}
			pair = base + 2;
		} else if (py_identifier((unsigned char)text[at])) {
			at++;
			while (at < size &&
				(py_identifier((unsigned char)text[at]) || (text[at] >= '0' && text[at] <= '9'))) {
				at++;
			}
			size_t next = at;
			while (next < size && (text[next] == ' ' || text[next] == '\t')) {
				next++;
			}
			if ((at - start == 4 &&
					(!memcmp(text + start, "True", 4) || !memcmp(text + start, "None", 4))) ||
				(at - start == 5 && !memcmp(text + start, "False", 5))) {
				pair = base + 2;
			} else if (py_keyword(text + start, at - start)) {
				pair = base;
			} else if (definition || (next < size && text[next] == '(')) {
				pair = base + 4;
			}
			definition = (at - start == 3 && !memcmp(text + start, "def", 3)) ||
				(at - start == 5 && !memcmp(text + start, "class", 5));
		} else {
			/* Decode a whole UTF-8 character rather than splitting its bytes. */
			mbstate_t mb = {0};
			wchar_t wc;
			size_t n = mbrtowc(&wc, text + at, size - at, &mb);
			if (n == (size_t)-2) {
				break;
			}
			at += n == (size_t)-1 || !n ? 1 : n;
			if (strchr("+-*/%=<>!&|^~:.,;()[]{}", text[start])) {
				pair = background == 6 ? 25 : 26;
			}
		}
		attr_t attr = COLOR_PAIR(pair);
#ifdef A_ITALIC
		if (pair == base + 3) {
			attr |= A_ITALIC;
		}
#endif
		md_text(c, text + start, at - start, attr, background);
	}
}

/* Small C-family lexer for write previews. Display only, deliberately not a parser. */
struct c_syntax_state {
	int block_comment;
	char quote;
};

static int c_identifier(unsigned char ch) {

	return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '_' || ch >= 128;
}

static int c_keyword(const char *text, size_t size) {

	static const char *words[] = {"_Alignas", "_Alignof", "_Atomic", "_Bool", "_Complex",
		"_Generic", "_Imaginary", "_Noreturn", "_Static_assert", "_Thread_local", "alignas",
		"alignof", "asm", "auto", "bool", "break", "case", "catch", "char", "class", "const",
		"constexpr", "continue", "default", "delete", "do", "double", "else", "enum", "explicit",
		"export", "extern", "false", "float", "for", "friend", "goto", "if", "inline", "int",
		"long", "namespace", "new", "noexcept", "nullptr", "operator", "private", "protected",
		"public", "register", "restrict", "return", "short", "signed", "sizeof", "static", "struct",
		"switch", "template", "this", "throw", "true", "try", "typedef", "typename", "union",
		"unsigned", "using", "virtual", "void", "volatile", "wchar_t", "while"};
	for (size_t i = 0; i < sizeof(words) / sizeof(*words); i++) {
		if (strlen(words[i]) == size && !memcmp(text, words[i], size)) {
			return 1;
		}
	}

	return 0;
}

static void md_c(struct md_canvas *c, const char *text, size_t size, int background,
	struct c_syntax_state *state) {

	int base = background == 6 ? 14 : 19;
	int plain = background == 6 ? 6 : 24;
	size_t at = 0;
	while (at < size) {
		size_t start = at;
		int pair = plain;
		if (state->block_comment) {
			while (at < size) {
				if (at + 1 < size && text[at] == '*' && text[at + 1] == '/') {
					at += 2;
					state->block_comment = 0;
					break;
				}
				at++;
			}
			pair = base + 3;
		} else if (state->quote || text[at] == '\'' || text[at] == '"') {
			char quote = state->quote ? state->quote : text[at++];
			state->quote = quote;
			while (at < size) {
				if (text[at] == '\\') {
					at += at + 1 < size ? 2 : 1;
					continue;
				}
				if (text[at++] == quote) {
					state->quote = 0;
					break;
				}
			}
			pair = base + 1;
		} else if (at + 1 < size && text[at] == '/' && text[at + 1] == '/') {
			at = size;
			pair = base + 3;
		} else if (at + 1 < size && text[at] == '/' && text[at + 1] == '*') {
			at += 2;
			state->block_comment = 1;
			while (at < size) {
				if (at + 1 < size && text[at] == '*' && text[at + 1] == '/') {
					at += 2;
					state->block_comment = 0;
					break;
				}
				at++;
			}
			pair = base + 3;
		} else if (text[at] == '#' && (at == 0 || text[at - 1] == ' ' || text[at - 1] == '\t')) {
			at++;
			while (at < size &&
				(c_identifier((unsigned char)text[at]) || (text[at] >= '0' && text[at] <= '9'))) {
				at++;
			}
			pair = base;
		} else if ((text[at] >= '0' && text[at] <= '9') ||
			(text[at] == '.' && at + 1 < size && text[at + 1] >= '0' && text[at + 1] <= '9')) {
			at++;
			while (at < size &&
				(c_identifier((unsigned char)text[at]) || (text[at] >= '0' && text[at] <= '9') ||
					strchr("._+-", text[at]))) {
				at++;
			}
			pair = base + 2;
		} else if (c_identifier((unsigned char)text[at])) {
			at++;
			while (at < size &&
				(c_identifier((unsigned char)text[at]) || (text[at] >= '0' && text[at] <= '9'))) {
				at++;
			}
			size_t next = at;
			while (next < size && (text[next] == ' ' || text[next] == '\t')) {
				next++;
			}
			if (c_keyword(text + start, at - start)) {
				pair = base;
			} else if (next < size && text[next] == '(') {
				pair = base + 4;
			}
		} else {
			mbstate_t mb = {0};
			wchar_t wc;
			size_t n = mbrtowc(&wc, text + at, size - at, &mb);
			if (n == (size_t)-2) {
				break;
			}
			at += n == (size_t)-1 || !n ? 1 : n;
			if (strchr("+-*/%=<>!&|^~?:.,;()[]{}", text[start])) {
				pair = background == 6 ? 25 : 26;
			}
		}
		attr_t attr = COLOR_PAIR(pair);
#ifdef A_ITALIC
		if (pair == base + 3) {
			attr |= A_ITALIC;
		}
#endif
		md_text(c, text + start, at - start, attr, background);
	}
}

/* Small shell lexer for display only: the command word of each command
   (after |, &&, ;, a keyword such as then), keywords, strings, comments,
   $variables and numbers, and -flags. A quote may run across lines. */
struct shell_state {
	char quote;
};

static int shell_keyword(const char *text, size_t size) {

	static const char *words[] = {"if", "then", "else", "elif", "fi", "for", "while", "until", "do",
		"done", "case", "esac", "in", "function", "select", "time", "return", "export", "local"};
	for (size_t i = 0; i < sizeof(words) / sizeof(*words); i++) {
		if (strlen(words[i]) == size && !memcmp(text, words[i], size)) {
			return 1;
		}
	}

	return 0;
}

static int shell_word(unsigned char ch) {

	return ch > ' ' && !strchr("|&;<>()'\"`$#", ch);
}

static void md_shell(
	struct md_canvas *c, const char *text, size_t size, int background, struct shell_state *state) {

	int base = background == 6 ? 14 : 19;
	int plain = background == 6 ? 6 : 24;
	int operator= background == 6 ? 25 : 26;
	int command = !state->quote; /* the next word names a command */
	size_t at = 0;
	while (at < size) {
		size_t start = at;
		int pair = plain;
		if (state->quote || text[at] == '\'' || text[at] == '"') {
			char quote = state->quote ? state->quote : text[at++];
			state->quote = quote;
			while (at < size) {
				if (quote == '"' && text[at] == '\\') {
					at += at + 1 < size ? 2 : 1;
					continue;
				}
				if (text[at++] == quote) {
					state->quote = 0;
					break;
				}
			}
			pair = base + 1;
			command = 0;
		} else if (text[at] == '#' && (at == 0 || text[at - 1] == ' ' || text[at - 1] == '\t')) {
			at = size;
			pair = base + 3;
		} else if (text[at] == '$') {
			at++;
			if (at < size && text[at] == '{') {
				while (at < size && text[at] != '}') {
					at++;
				}
				if (at < size) {
					at++;
				}
			} else if (at < size && text[at] == '(') {
				at++;
				command = 1; /* $( starts a command */
				pair = operator;
				md_text(c, text + start, at - start, COLOR_PAIR(pair), background);
				continue;
			} else {
				while (at < size &&
					(c_identifier((unsigned char)text[at]) ||
						(text[at] >= '0' && text[at] <= '9') || strchr("?@#*!-", text[at]))) {
					at++;
					if (strchr("?@#*!-", text[at - 1])) {
						break;
					}
				}
			}
			pair = base + 2;
			command = 0;
		} else if (strchr("|&;()<>", text[at])) {
			while (at < size && strchr("|&;()<>", text[at])) {
				at++;
			}
			pair = operator;
			/* A redirection names a file, not a command. */
			command =
				!memchr(text + start, '>', at - start) && !memchr(text + start, '<', at - start);
		} else if (text[at] == ' ' || text[at] == '\t') {
			while (at < size && (text[at] == ' ' || text[at] == '\t')) {
				at++;
			}
		} else if (shell_word((unsigned char)text[at])) {
			while (at < size && shell_word((unsigned char)text[at])) {
				at++;
			}
			int keyword = shell_keyword(text + start, at - start);
			if (keyword) {
				pair = base;
				/* if, then, do, else ... are followed by a command; for,
                   case, in, export ... by a name or a value. */
				static const char *leads[] = {
					"if", "then", "else", "elif", "do", "while", "until", "time"};
				command = 0;
				for (size_t i = 0; i < sizeof(leads) / sizeof(*leads); i++) {
					if (strlen(leads[i]) == at - start &&
						!memcmp(text + start, leads[i], at - start)) {
						command = 1;
					}
				}
			} else if (command && !memchr(text + start, '=', at - start)) {
				pair = base + 4;
				command = 0;
			} else if (text[start] == '-') {
				pair = operator;
			} else if (text[start] >= '0' && text[start] <= '9') {
				size_t digits = start;
				while (digits < at && text[digits] >= '0' && text[digits] <= '9') {
					digits++;
				}
				if (digits == at) {
					pair = base + 2;
				}
			}
		} else {
			mbstate_t mb = {0};
			wchar_t wc;
			size_t n = mbrtowc(&wc, text + at, size - at, &mb);
			if (n == (size_t)-2) {
				break;
			}
			at += n == (size_t)-1 || !n ? 1 : n;
		}
		attr_t attr = COLOR_PAIR(pair);
#ifdef A_ITALIC
		if (pair == base + 3) {
			attr |= A_ITALIC;
		}
#endif
		md_text(c, text + start, at - start, attr, background);
	}
}
