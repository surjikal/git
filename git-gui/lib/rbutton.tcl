# git-gui rounded push button
# Copyright (C) 2026 git-gui contributors

# Drop-in replacement for ttk::button, built on a plain canvas.
# ttk's clam theme (used for the dark theme) can only draw square-cornered
# buttons, so plain clicky actions get their corners rounded here instead.
# Supports the option subset git-gui actually uses on buttons: -text,
# -command, -state, -default and -width. Anything else is passed straight
# through to the underlying canvas widget.
#
# Deliberately does not draw a focus ring: Tab still moves focus to the
# button and Return/space still activates it, there's just no highlight
# drawn for that state, trading a bit of keyboard-accessibility polish for
# a much smaller/simpler implementation.

set rbutton_padx 16
set rbutton_pady 9
# Tk's canvas -smooth spline only bulges the curve in close to each raw
# point rather than sweeping the full stated radius, so small radii (~6px)
# render as visually sharp corners on screen even though the control points
# are mathematically correct (confirmed via postscript export). 10px is
# the smallest value that reliably reads as "rounded" at typical button
# heights (~30px).
set rbutton_radius 10

proc rbutton {w args} {
	global color_bg

	canvas $w -highlightthickness 0 -borderwidth 0 -background $color_bg \
		-takefocus 1

	upvar #0 rbutton_opt_$w opt
	array set opt {
		-text {} -command {} -state normal -default normal -width {}
		hover 0 pressed 0
	}

	rename $w _$w
	interp alias {} $w {} rbutton_widgetproc $w

	bind $w <Configure>       [list rbutton_redraw $w]
	bind $w <Enter>           [list rbutton_hover $w 1]
	bind $w <Leave>           [list rbutton_hover $w 0]
	bind $w <ButtonPress-1>   [list rbutton_press $w 1]
	bind $w <ButtonRelease-1> [list rbutton_press $w 0]
	bind $w <Return>          [list rbutton_invoke $w]
	bind $w <KP_Enter>        [list rbutton_invoke $w]
	bind $w <space>           [list rbutton_invoke $w]
	bind $w <Destroy>         [list rbutton_cleanup $w]

	eval [linsert $args 0 rbutton_widgetproc $w configure]
	rbutton_natural_size $w
	rbutton_redraw $w
	return $w
}

proc rbutton_widgetproc {w cmd args} {
	upvar #0 rbutton_opt_$w opt
	switch -- $cmd {
		configure {
			set resize 0
			set redraw 0
			foreach {k v} $args {
				switch -- $k {
					-text    { set opt(-text) $v;    set resize 1; set redraw 1 }
					-width   { set opt(-width) $v;   set resize 1; set redraw 1 }
					-state   { set opt(-state) $v;   set redraw 1 }
					-default { set opt(-default) $v; set redraw 1 }
					-command { set opt(-command) $v }
					default  { _$w configure $k $v }
				}
			}
			if {$resize} { rbutton_natural_size $w }
			if {$redraw} { rbutton_redraw $w }
			return {}
		}
		cget {
			set k [lindex $args 0]
			if {[info exists opt($k)]} { return $opt($k) }
			return [uplevel 1 [list _$w cget $k]]
		}
		invoke {
			return [rbutton_invoke $w]
		}
		default {
			return [uplevel 1 [list _$w $cmd] $args]
		}
	}
}

proc rbutton_natural_size {w} {
	global rbutton_padx rbutton_pady
	upvar #0 rbutton_opt_$w opt

	set width  [expr {[font measure font_ui $opt(-text)] + 2 * $rbutton_padx}]
	set height [expr {[font metrics font_ui -linespace] + 2 * $rbutton_pady}]

	if {$opt(-width) ne {} && $opt(-width) > 0} {
		set minw [expr {
			$opt(-width) * [font measure font_ui "0"] + 2 * $rbutton_padx
		}]
		if {$minw > $width} { set width $minw }
	}

	_$w configure -width $width -height $height
}

proc rbutton_hover {w on} {
	upvar #0 rbutton_opt_$w opt
	set opt(hover) $on
	rbutton_redraw $w
}

proc rbutton_press {w on} {
	upvar #0 rbutton_opt_$w opt
	if {$opt(-state) eq {disabled}} { return }

	if {$on} {
		set opt(pressed) 1
		rbutton_redraw $w
		return
	}

	if {!$opt(pressed)} { return }
	set opt(pressed) 0
	rbutton_redraw $w

	set px [winfo pointerx $w]
	set py [winfo pointery $w]
	set wx [winfo rootx $w]
	set wy [winfo rooty $w]
	if {$px >= $wx && $px < $wx + [winfo width $w] \
	 && $py >= $wy && $py < $wy + [winfo height $w]} {
		rbutton_invoke $w
	}
}

proc rbutton_invoke {w} {
	upvar #0 rbutton_opt_$w opt
	if {$opt(-state) eq {disabled} || $opt(-command) eq {}} { return }
	uplevel #0 $opt(-command)
}

proc rbutton_cleanup {w} {
	if {[info exists ::rbutton_opt_$w]} {
		unset ::rbutton_opt_$w
	}
}

proc rbutton_redraw {w} {
	global color_bg_overlay color_bg_active color_bg_panel \
		color_border color_border_emphasis color_accent_emphasis \
		color_fg color_fg_subtle rbutton_radius
	upvar #0 rbutton_opt_$w opt

	_$w delete all

	set width  [winfo width $w]
	set height [winfo height $w]
	if {$width  <= 1} { set width  [_$w cget -width] }
	if {$height <= 1} { set height [_$w cget -height] }

	set disabled [expr {$opt(-state) eq {disabled}}]
	set pressed  [expr {!$disabled && $opt(pressed)}]
	set hover    [expr {!$disabled && !$pressed && $opt(hover)}]

	if {$disabled} {
		set fill   $color_bg_panel
		set border $color_border
		set fg     $color_fg_subtle
	} elseif {$pressed} {
		set fill   $color_border
		set border $color_border_emphasis
		set fg     $color_fg
	} elseif {$hover} {
		set fill   $color_bg_active
		set border $color_border_emphasis
		set fg     $color_fg
	} else {
		set fill   $color_bg_overlay
		set border $color_border_emphasis
		set fg     $color_fg
	}
	if {$opt(-default) eq {active} && !$disabled} {
		set border $color_accent_emphasis
	}

	set r [expr {min($rbutton_radius, int($width / 2), int($height / 2))}]
	rbutton_round_rect $w 1 1 [expr {$width - 1}] [expr {$height - 1}] $r \
		-fill $fill -outline $border -width 1

	_$w create text [expr {$width / 2}] [expr {$height / 2}] \
		-text $opt(-text) -fill $fg -font font_ui -anchor center
}

# Draws a rounded rectangle as a single smoothed polygon: Tk canvas has no
# native rounded-rect primitive, but a 12-point polygon with -smooth 1
# rounds off each corner closely enough for a button chrome.
proc rbutton_round_rect {w x0 y0 x1 y1 r args} {
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
