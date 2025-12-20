# ---------------------------------------------
# COLOSSUS Editor Makefile
# ---------------------------------------------

CXX      := g++
CXXFLAGS := -std=c++17 -O2 -Wall -Wextra -g

PKGCONF ?= pkg-config
PKG     := gtk+-3.0 gtksourceview-3.0

INCLUDES := $(shell $(PKGCONF) --cflags $(PKG))
LIBS     := $(shell $(PKGCONF) --libs $(PKG))

TARGET   := editor
SRC      := main.cpp editor.cpp
OBJ      := $(SRC:.cpp=.o)

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CXX) $(CXXFLAGS) $(OBJ) $(LIBS) -o $(TARGET)

%.o: %.cpp editor.h
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

clean:
	rm -f $(OBJ) $(TARGET)

run: all
	./$(TARGET)
