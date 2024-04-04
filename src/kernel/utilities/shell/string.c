#include <string.h>
#include <util.h>

// String management file
// Copyright (C) 2024 Panagiotis

uint32_t strlength(char *ch) {
  uint32_t i = 0; // Changed counter to 0
  while (ch[i++])
    ;
  return i - 1; // Changed counter to i instead of i--
}

int atoi(const char *str) {
  int value = 0;
  while (isdigit(*str)) {
    value *= 10;
    value += (*str) - '0';
    str++;
  }

  return value;
}

bool check_string(char *str) { return (str[0] != 0x0); }

bool strEql(char *ch1, char *ch2) {
  uint32_t size = strlength(ch1);

  if (size != strlength(ch2))
    return false;

  for (uint32_t i = 0; i <= size; i++) {
    if (ch1[i] != ch2[i])
      return false;
  }

  return true;
}

char *strpbrk(const char *str, const char *delimiters) {
  while (*str) {
    const char *d = delimiters;
    while (*d) {
      if (*d == *str) {
        return (char *)str;
      }
      ++d;
    }
    ++str;
  }
  return NULL;
}

char *strtok(char *str, const char *delimiters, char **context) {
  if (str == NULL && *context == NULL)
    return NULL;

  if (str != NULL)
    *context = str;

  char *token_start = *context;
  char *token_end = strpbrk(token_start, delimiters);

  if (token_end != NULL) {
    *token_end = '\0';
    *context = token_end + 1;
    return token_start;
  } else if (*token_start != '\0') {
    *context = NULL;
    return token_start;
  }

  return NULL;
}

long strtol(const char *s, char **endptr, int base) {
  int  neg = 0;
  long val = 0;

  // gobble initial whitespace
  while (*s == ' ' || *s == '\t') {
    s++;
  }

  // plus/minus sign
  if (*s == '+') {
    s++;
  } else if (*s == '-') {
    s++, neg = 1;
  }

  // hex or octal base prefix
  if ((base == 0 || base == 16) && (s[0] == '0' && s[1] == 'x')) {
    s += 2, base = 16;
  } else if (base == 0 && s[0] == '0') {
    s++, base = 8;
  } else if (base == 0) {
    base = 10;
  }

  // digits
  while (1) {
    int dig;

    if (*s >= '0' && *s <= '9') {
      dig = *s - '0';
    } else if (*s >= 'a' && *s <= 'z') {
      dig = *s - 'a' + 10;
    } else if (*s >= 'A' && *s <= 'Z') {
      dig = *s - 'A' + 10;
    } else {
      break;
    }
    if (dig >= base) {
      break;
    }
    s++, val = (val * base) + dig;
    // we don't properly detect overflow!
  }

  if (endptr) {
    *endptr = (char *)s;
  }
  return (neg ? -val : val);
}
