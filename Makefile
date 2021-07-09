#
# Copyright (C) 2021 Linzhi Ltd.
#
# This work is licensed under the terms of the MIT License.
# A copy of the license can be found in the file COPYING.txt
#

NAME = dagd

CFLAGS = -g -Wall -Wextra -Wshadow -Wno-unused-parameter \
         -Wmissing-prototypes -Wmissing-declarations \
         -I../../lib/common -I../../lib/dag \
         -I../libcommon -I../libdag
LDLIBS = -L../../lib/dag -L../libdag -L../lib -ldag \
    -L../../lib/common -L../libcommon -lcommon -lmosquitto -lgcrypt -lm

OBJS = $(NAME).o epoch.o cache.o dag.o debug.o mqtt.o csum.o

include Makefile.c-common


.PHONY:		all spotless

all::		| $(OBJDIR:%/=%)
all::		$(OBJDIR)$(NAME)

$(OBJDIR:%/=%):
		mkdir -p $@

$(OBJDIR)$(NAME): $(OBJS_IN_OBJDIR)

spotless::
		rm -f $(OBJDIR)$(NAME)
