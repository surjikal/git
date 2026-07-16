# git-gui rounded checkbutton
# Copyright (C) 2026 git-gui contributors

# Drop-in replacement for ttk::checkbutton, built on a plain canvas.
# ttk's clam theme draws its checkbox indicator at a fixed, small size that
# isn't adjustable through style options (-indicatormargin only changes the
# spacing around it, not the glyph itself), so a canvas widget is used here
# for a checkbox that's actually legible next to the rest of the dark theme.
#
# Supports the option subset git-gui actually uses on checkbuttons: -text,
# -variable, -onvalue, -offvalue, -command and -state. Anything else (e.g.
# -background, to sit on a Toolbar panel) is passed straight through to the
# underlying canvas widget.
#
# Like rbutton, this deliberately does not draw a focus ring.

set rcheckbutton_box    16
set rcheckbutton_gap    8
set rcheckbutton_radius 4
set rcheckbutton_padx   2
set rcheckbutton_pady   2

proc rcheckbutton {w args} {
	global color_bg

	canvas $w -highlightthickness 0 -borderwidth 0 -background $color_bg \
		-takefocus 1

	upvar #0 rcb_opt_$w opt
	array set opt {
		-text {} -variable {} -onvalue 1 -offvalue 0 -command {}
		-state normal
		hover 0
	}

	rename $w _$w
	interp alias {} $w {} rcb_widgetproc $w

	bind $w <Configure>       [list rcb_redraw $w]
	bind $w <Enter>           [list rcb_hover $w 1]
	bind $w <Leave>           [list rcb_hover $w 0]
	bind $w <ButtonRelease-1> [list rcb_click $w]
	bind $w <Return>          [list rcb_click $w]
	bind $w <space>           [list rcb_click $w]
	bind $w <Destroy>         [list rcb_cleanup $w]

	eval [linsert $args 0 rcb_widgetproc $w configure]
	rcb_size $w
	rcb_redraw $w
	return $w
}

proc rcb_widgetproc {w cmd args} {
	upvar #0 rcb_opt_$w opt
	switch -- $cmd {
		configure {
			set resize 0
			set redraw 0
			foreach {k v} $args {
				switch -- $k {
					-text     { set opt(-text) $v;     set resize 1; set redraw 1 }
					-state    { set opt(-state) $v;    set redraw 1 }
					-onvalue  { set opt(-onvalue) $v;  set redraw 1 }
					-offvalue { set opt(-offvalue) $v; set redraw 1 }
					-command  { set opt(-command) $v }
					-variable {
						# uplevel #0 so a bare (non ::-qualified) name resolves
						# against the real global variable, matching the
						# `upvar #0` used everywhere else this option is read
						# -- otherwise the trace silently attaches to a local
						# variable of this proc's own frame and never fires.
						if {$opt(-variable) ne {}} {
							catch {
								uplevel #0 [list trace remove variable \
									$opt(-variable) write [list rcb_syncredraw $w]]
							}
						}
						set opt(-variable) $v
						uplevel #0 [list trace add variable \
							$opt(-variable) write [list rcb_syncredraw $w]]
						set redraw 1
					}
					default   { _$w configure $k $v }
				}
			}
			if {$resize} { rcb_size $w }
			if {$redraw} { rcb_redraw $w }
			return {}
		}
		cget {
			set k [lindex $args 0]
			if {[info exists opt($k)]} { return $opt($k) }
			return [uplevel 1 [list _$w cget $k]]
		}
		invoke {
			return [rcb_click $w]
		}
		default {
			return [uplevel 1 [list _$w $cmd] $args]
		}
	}
}

proc rcb_size {w} {
	global rcheckbutton_box rcheckbutton_gap rcheckbutton_padx rcheckbutton_pady
	upvar #0 rcb_opt_$w opt

	set tw [font measure font_ui $opt(-text)]
	set lh [font metrics font_ui -linespace]
	set width  [expr {
		2 * $rcheckbutton_padx + $rcheckbutton_box + $rcheckbutton_gap + $tw
	}]
	set height [expr {2 * $rcheckbutton_pady + max($rcheckbutton_box, $lh)}]

	_$w configure -width $width -height $height
}

proc rcb_hover {w on} {
	upvar #0 rcb_opt_$w opt
	set opt(hover) $on
	rcb_redraw $w
}

proc rcb_checked {w} {
	upvar #0 rcb_opt_$w opt
	if {$opt(-variable) eq {}} { return 0 }
	upvar #0 $opt(-variable) v
	return [expr {[info exists v] && $v eq $opt(-onvalue)}]
}

proc rcb_click {w} {
	upvar #0 rcb_opt_$w opt
	if {$opt(-state) eq {disabled} || $opt(-variable) eq {}} { return }
	upvar #0 $opt(-variable) v
	if {[rcb_checked $w]} {
		set v $opt(-offvalue)
	} else {
		set v $opt(-onvalue)
	}
	# Redraw directly rather than relying solely on the write trace: this is
	# what makes the widget's own click responsive even in the (unexpected)
	# case the trace doesn't fire. The trace still exists to catch external
	# changes to the variable (e.g. a keyboard shortcut toggling it).
	rcb_redraw $w
	if {$opt(-command) ne {}} {
		uplevel #0 $opt(-command)
	}
}

# Fired via variable trace: keeps the drawn state in sync whenever the
# backing variable changes, whether from this widget's own click handler or
# from other code toggling it directly (e.g. loading saved state).
proc rcb_syncredraw {w args} {
	if {[winfo exists $w]} { rcb_redraw $w }
}

proc rcb_cleanup {w} {
	upvar #0 rcb_opt_$w opt
	if {[info exists opt(-variable)] && $opt(-variable) ne {}} {
		catch {trace remove variable $opt(-variable) write [list rcb_syncredraw $w]}
	}
	if {[info exists ::rcb_opt_$w]} {
		unset ::rcb_opt_$w
	}
}

proc rcb_redraw {w} {
	global color_bg_overlay color_bg_active color_bg_panel \
		color_border color_border_emphasis color_accent_emphasis \
		color_fg color_fg_subtle rcheckbutton_box rcheckbutton_radius \
		rcheckbutton_padx
	upvar #0 rcb_opt_$w opt

	_$w delete all

	set height [winfo height $w]
	if {$height <= 1} { set height [_$w cget -height] }

	set disabled [expr {$opt(-state) eq {disabled}}]
	set checked  [rcb_checked $w]
	set hover    [expr {!$disabled && $opt(hover)}]

	if {$disabled} {
		set border $color_border
		set fg     $color_fg_subtle
		set fill   [expr {$checked ? $color_bg_panel : $color_bg_panel}]
		set mark   $color_fg_subtle
	} elseif {$checked} {
		set border $color_accent_emphasis
		set fg     $color_fg
		set fill   $color_accent_emphasis
		set mark   $color_fg
	} else {
		set border [expr {$hover ? $color_border_emphasis : $color_border_emphasis}]
		set fg     $color_fg
		set fill   [expr {$hover ? $color_bg_active : $color_bg_overlay}]
		set mark   {}
	}

	set bx0 $rcheckbutton_padx
	set by0 [expr {($height - $rcheckbutton_box) / 2}]
	set bx1 [expr {$bx0 + $rcheckbutton_box}]
	set by1 [expr {$by0 + $rcheckbutton_box}]

	rcb_round_rect $w $bx0 $by0 $bx1 $by1 $rcheckbutton_radius \
		-fill $fill -outline $border -width 1

	if {$checked} {
		_$w create line \
			[expr {$bx0 + 3}] [expr {$by0 + 8}] \
			[expr {$bx0 + 6}] [expr {$by0 + 11}] \
			[expr {$bx0 + 13}] [expr {$by0 + 4}] \
			-fill $mark -width 2 -joinstyle round -capstyle round
	}

	_$w create text [expr {$bx1 + 8}] [expr {$height / 2}] \
		-text $opt(-text) -fill $fg -font font_ui -anchor w
}

proc rcb_round_rect {w x0 y0 x1 y1 r args} {
	if {$r > ($x1 - $x0) / 2} { set r [expr {($x1 - $x0) / 2}] }
	if {$r > ($y1 - $y0) / 2} { set r [expr {($y1 - $y0) / 2}] }
	set pts [list \
		[expr {$x0 + $r}] $y0 \
		[expr {$x1 - $r}] $y0 \
		$x1 $y0 \
		$x1 [expr {$y0 + $r}] \
		$x1 [expr {$y1 - $r}] \
		$x1 $y1 \
		[expr {$x1 - $r}] $y1 \
		[expr {$x0 + $r}] $y1 \
		$x0 $y1 \
		$x0 [expr {$y1 - $r}] \
		$x0 [expr {$y0 + $r}] \
		$x0 $y0 \
	]
	return [_$w create polygon $pts -smooth 1 {*}$args]
}

# Local variables:
# mode: tcl
# indent-tabs-mode: t
# tab-width: 4
# End:
