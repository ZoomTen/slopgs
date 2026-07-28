# Makefile -- renders CHART.adoc to an A4, two-column PDF via asciidoctor-pdf.
# Separate from the root Makefile (which only builds the synth engine).

ASCIIDOCTOR_PDF ?= asciidoctor-pdf

.PHONY: all clean

all: CHART.pdf

CHART.pdf: CHART.adoc chart-theme.yml admonition-topbox.rb
	$(ASCIIDOCTOR_PDF) -r ./admonition-topbox.rb -a pdf-theme=chart-theme.yml -o $@ $<

clean:
	rm -f CHART.pdf
