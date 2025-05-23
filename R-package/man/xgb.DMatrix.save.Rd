% Generated by roxygen2: do not edit by hand
% Please edit documentation in R/xgb.DMatrix.save.R
\name{xgb.DMatrix.save}
\alias{xgb.DMatrix.save}
\title{Save xgb.DMatrix object to binary file}
\usage{
xgb.DMatrix.save(dmatrix, fname)
}
\arguments{
\item{dmatrix}{the \code{xgb.DMatrix} object}

\item{fname}{the name of the file to write.}
}
\description{
Save xgb.DMatrix object to binary file
}
\examples{
\dontshow{RhpcBLASctl::omp_set_num_threads(1)}
data(agaricus.train, package = "xgboost")

dtrain <- with(agaricus.train, xgb.DMatrix(data, label = label, nthread = 2))
fname <- file.path(tempdir(), "xgb.DMatrix.data")
xgb.DMatrix.save(dtrain, fname)
dtrain <- xgb.DMatrix(fname, nthread = 1)
}
