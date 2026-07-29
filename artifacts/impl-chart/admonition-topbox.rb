# Chart's converter overrides (plural). Currently:
#  - convert_admonition: replaces asciidoctor-pdf's default admonition layout
#    (label in a left column, vertical rule, content to the right) with a
#    bordered box whose label sits on its own line at the top -- content
#    fills the full box width below it.
#  - convert_list: restores the trailing block margin the gem suppresses for
#    a list nested inside a dlist description.
# Loaded via `-r` in the Makefile.
#
# ponytail: filename now narrower than its contents (topbox is just the
# admonition override); not worth a Makefile-touching rename.
#
# ponytail: convert_admonition assumes text labels (no admonition icons) and
# blocks that don't split across pages -- true for every admonition in
# CHART.adoc. Add icon/pagination handling back from the stock
# #convert_admonition (in the asciidoctor-pdf gem) if either is ever needed.
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
    # Restore the stock method's trailing margin, which this override had
    # dropped -- without it, admonition boxes get zero bottom margin.
    theme_margin :block, :bottom, (next_enclosed_block node)
  end

  # gem's convert_list (converter.rb:1622-1658) skips the trailing prose
  # margin `unless node.nested?`, so a bullet list hanging off a dlist
  # description (e.g. a CC parameter's value bullets) glues onto the next
  # dlist term. Restore it for that one case; leave other nested lists (e.g.
  # inside an ordinary list item) suppressed as the gem intends.
  #
  # Exclude a compound last item: convert_list_item (converter.rb:1741-1747)
  # already deletes margin_bottom for a compound item, letting its inner
  # content emit its own prose_margin_bottom. Adding ours on top double-counts
  # that margin.
  def convert_list node
    super
    return unless node.nested? && (dd = node.parent).context == :list_item && dd.parent.context == :dlist
    return if node.items[-1].compound?
    theme_margin :prose, :bottom, (next_enclosed_block node)
  end
end)
