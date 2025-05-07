# Compiler flags:
# -Wall: Enable all common compiler warnings
# -Wextra: Enable additional warnings beyond -Wall
# -g: Include debugging symbols in the compiled binary
# -std=gnu99: Use the GNU C99 standard (for backward compatibility with older systems)
# -U_FORTIFY_SOURCE: Disable source fortification which may require newer GLIBC versions (GLIBC on submission platform is GLIBC 2.34)
CFLAGS = -Wall -Wextra -g -std=gnu99 -U_FORTIFY_SOURCE

# Linker flags:
# -lbsd: Link against the BSD compatibility library
# -Wl,--hash-style=both: Pass "--hash-style=both" to the linker for compatibility with older systems
# -static-libgcc: Statically link libgcc to avoid runtime dependencies on newer GLIBC
LDFLAGS = -lbsd -Wl,--hash-style=both -static-libgcc

# Include directory path
# -Iinclude: Look for header files in the "include" directory
INCLUDE = -Iinclude

# List of source files to compile
SRCS = main.c fs.c error.c vdisk/vdisk.c

# Generate object file names by replacing .c with .o in SRCS
OBJS = $(SRCS:.c=.o)

# Name of the output executable
TARGET = fs_test

# Default make target - builds the executable from object files
all: $(OBJS)
	gcc -o $(TARGET) $(OBJS) $(LDFLAGS)

# Pattern rule to compile each .c file into a .o object file
# $< refers to the prerequisite (the .c file)
# $@ refers to the target (the .o file)
%.o: %.c
	$(CC) $(CFLAGS) $(INCLUDE) -c $< -o $@

# Target to remove all compiled files
clean:
	rm -f $(OBJS) $(TARGET)

# Special target that doesn't correspond to files (prevents conflicts with files named "all" or "clean")
.PHONY: all clean