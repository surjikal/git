# git-gui rounded entry
# Copyright (C) 2026 git-gui contributors

# Drop-in-ish replacement for ttk::entry, styled to match rbutton/
# rcheckbutton's rounded chip look. Actual text editing is NOT
# reimplemented -- a real ttk::entry is embedded inside the canvas via
# `create window`, flattened (no border/relief of its own) so it reads as
# sitting inside the rounded shape. Every option/subcommand other than
# -background is forwarded straight through to that inner entry, so this
# behaves like a normal entry for -textvariable, get/icursor/selection/etc.
#
# Because the real widget is $w.e (a child window), code that needs to
# `focus` this entry should target "$w.e", not "$w" -- `focus` is a plain
# Tk command, not something that can be intercepted via command aliasing
# the way configure/cget are here.

set rentry_padx   16
set rentry_pady   14
set rentry_radius 10

proc rentry {w args} {
	global color_bg color_bg_overlay color_fg color_accent rentry_padx

	rentry_ensure_style

	canvas $w -highlightthickness 0 -borderwidth 0 -background $color_bg \
		-takefocus 0

	upvar #0 rentry_opt_$w opt
	array set opt {focused 0}

	# -font is also set directly on the widget (belt-and-suspenders on top
	# of Rentry.TEntry's own -font font_ui) since relying purely on style
	# cascade timing here has bitten this file once already.
	ttk::entry $w.e -style Rentry.TEntry -font font_ui
	bind $w.e <FocusIn>  [list rentry_focus $w 1]
	bind $w.e <FocusOut> [list rentry_focus $w 0]

	rename $w _$w
	interp alias {} $w {} rentry_widgetproc $w

	# Canvas defaults to a "10c"-style physical-unit size when never given
	# an explicit -width/-height, which breaks the plain-integer pixel math
	# in rentry_redraw -- give it a concrete pixel size up front, same as
	# rbutton does for its own canvas. Must run after the rename/alias
	# above since it goes through _$w.
	rentry_natural_size $w

	bind $w <Configure> [list rentry_redraw $w]
	bind $w <ButtonRelease-1> [list focus $w.e]
	bind $w <Destroy> [list rentry_cleanup $w]

	eval [linsert $args 0 rentry_widgetproc $w configure]
	rentry_redraw $w
	return $w
}

# The inner entry's own chrome is turned off (flat/borderless/matching
# fill) so only the canvas's rounded rect reads as the control's edge.
# ttk::style configure is idempotent, so no guard is needed against calling
# this once per rentry instance.
proc rentry_ensure_style {} {
	global color_bg_overlay color_fg
	ttk::style configure Rentry.TEntry \
		-fieldbackground $color_bg_overlay \
		-foreground $color_fg \
		-font font_ui \
		-borderwidth 0 \
		-bordercolor $color_bg_overlay \
		-lightcolor $color_bg_overlay \
		-darkcolor $color_bg_overlay \
		-padding 0
	# clam's base TEntry style maps -lightcolor/-bordercolor to light blues
	# on focus (its own default focus-ring look); without clearing every
	# one of those per-state overrides here, that leftover clam styling
	# still peeks through as a stray bright border while typing, on top of
	# rentry's own rounded canvas outline.
	ttk::style map Rentry.TEntry \
		-bordercolor {} \
		-lightcolor {} \
		-darkcolor {} \
		-fieldbackground {} \
		-background {}
}

proc rentry_natural_size {w} {
	global rentry_padx rentry_pady
	set width  160
	set height [expr {[font metrics font_ui -linespace] + 2 * $rentry_pady}]
	_$w configure -width $width -height $height
}

proc rentry_widgetproc {w cmd args} {
	switch -- $cmd {
		configure {
			foreach {k v} $args {
				switch -- $k {
					-background { _$w configure -background $v }
					default     { $w.e configure $k $v }
				}
			}
			return {}
		}
		cget {
			set k [lindex $args 0]
			if {$k eq {-background}} { return [uplevel 1 [list _$w cget $k]] }
			return [uplevel 1 [list $w.e cget $k]]
		}
		default {
			return [uplevel 1 [list $w.e $cmd] $args]
		}
	}
}

proc rentry_focus {w on} {
	upvar #0 rentry_opt_$w opt
	set opt(focused) $on
	rentry_redraw $w
}

proc rentry_cleanup {w} {
	if {[info exists ::rentry_opt_$w]} {
		unset ::rentry_opt_$w
	}
}

proc rentry_redraw {w} {
	global color_bg_overlay color_border_emphasis color_accent \
		rentry_radius rentry_padx rentry_pady
	upvar #0 rentry_opt_$w opt

	set width  [winfo width $w]
	set height [winfo height $w]
	if {$width  <= 1} { set width  [_$w cget -width] }
	if {$height <= 1} { set height [_$w cget -height] }

	set border [expr {$opt(focused) ? $color_accent : $color_border_emphasis}]

	_$w delete bg
	rentry_round_rect $w 1 1 [expr {$width - 1}] [expr {$height - 1}] $rentry_radius \
		-fill $color_bg_overlay -outline $border -width 1 -tags bg
	_$w lower bg

	set iw [expr {max(0, $width - 2 * $rentry_padx)}]
	if {[_$w find withtag entrywin] eq {}} {
		_$w create window $rentry_padx [expr {$height / 2}] \
			-anchor w -window $w.e -width $iw -tags entrywin
	} else {
		_$w coords entrywin $rentry_padx [expr {$height / 2}]
		_$w itemconfigure entrywin -width $iw
	}
}

proc rentry_round_rect {w x0 y0 x1 y1 r args} {
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
