# Replaces asciidoctor-pdf's default admonition layout (label in a left
# column, vertical rule, content to the right) with a bordered box whose
# label sits on its own line at the top -- content fills the full box width
# below it. Loaded via `-r` in the Makefile; only overrides the one method.
#
# ponytail: assumes text labels (no admonition icons) and blocks that don't
# split across pages -- true for every admonition in CHART.adoc. Add
# icon/pagination handling back from the stock #convert_admonition (in the
# asciidoctor-pdf gem) if either is ever needed.
Asciidoctor::PDF::Converter.prepend(Module.new do
  def convert_admonition node
    type = node.attr 'name'
    label_text = sanitize node.caption
    cpad = expand_padding_value @theme.admonition_padding

    bg_color = @theme[%(admonition_#{type}_background_color)] || @theme.admonition_background_color

    arrange_block node do |extent|
      add_dest_for_block node if node.id
      theme_fill_and_stroke_block :admonition, extent, background_color: bg_color if extent
      pad_box cpad do
        theme_font_cascade [:admonition_label, %(admonition_label_#{type})] do
          label_text = transform_text label_text, @text_transform if @text_transform
          ink_prose label_text, align: :left, line_height: 1, margin: 0, inline_format: false
        end
        move_down (@theme.admonition_label_padding&.dig(2) || (@theme.base_line_height_length / 3.0))
        ink_caption node, category: :admonition, labeled: false if node.title?
        theme_font :admonition do
          traverse node
        end
      end
    end
  end
end)
