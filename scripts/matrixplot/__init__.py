"""Read-benchmark pivot heatmaps + wall-time bottleneck breakdown.

Importing the package selects matplotlib's headless backend and the DUNE plot
style, so every figure in the package inherits both and submodules can import
pyplot freely (write files, never open a window).
"""
import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402  (must follow the backend choice)
import mplhep as hep  # noqa: E402

hep.style.use("DUNE")

# The style is sized for one large standalone plot. autolayout is the one that
# breaks things: left True it overrides the subplots_adjust the labels rely on.
plt.rcParams.update({
    "figure.autolayout": False,
    "figure.figsize": (9.0, 4.0),
    "font.size": 9,
    "axes.titlesize": 9,
    "axes.labelsize": 9,
    "xtick.labelsize": 8,
    "ytick.labelsize": 8,
    "axes.linewidth": 0.6,
    "axes.xmargin": 0.03,
    "xtick.minor.visible": False,
    "ytick.minor.visible": False,
    "xtick.major.size": 3,
    "ytick.major.size": 3,
    "savefig.dpi": 300,
    "pdf.fonttype": 42,
    "ps.fonttype": 42,
})
