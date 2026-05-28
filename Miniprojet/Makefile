SUBDIRS = deamon cli temperature_module

all:
	$(foreach dir, $(SUBDIRS), $(MAKE) -C $(dir);)

clean:
	$(foreach dir, $(SUBDIRS), $(MAKE) -C $(dir) clean;)

.PHONY: all clean